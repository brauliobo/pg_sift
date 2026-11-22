/*
 * agg/v6/v6_fuse_keys.c - the lead key of arms that fuse with different scan keys (v6_fuse_keys.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_oper.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_filter_eval.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_vexpr.h"
#include "agg/v6/v6_fuse_keys.h"
#include "col/am.h"
#include "col/f2/f2.h"
#include "core/errctx.h"

static bool
int_of(const Node *e, int64 *v)
{
	const Const *k = (const Const *) e;

	if (e == NULL || !IsA(e, Const) || k->constisnull)
		return false;
	switch (k->consttype)
	{
		case INT2OID:
			return (*v = DatumGetInt16(k->constvalue), true);
		case INT4OID:
		case DATEOID:
			return (*v = DatumGetInt32(k->constvalue), true);
		case INT8OID:
			return (*v = DatumGetInt64(k->constvalue), true);
		default:
			return false;
	}
}

static bool
is_equality(Oid opno)
{
	const char *name = get_opname(opno);

	return name != NULL && strcmp(name, "=") == 0;
}

static bool
add_value(SiftF2KeySet *ks, int64 v)
{
	if (ks->n == SIFT_F2_MAX_RANGES || v == PG_INT64_MAX)
		return false;
	ks->range[ks->n++] = (SiftF2KeyRange) {.has_lo = true,.has_hi = true,.ilo = v,.ihi = v + 1};
	return true;
}

/* the values the one qual allows on the lead key: an equality with a constant or a list of them; false for any other qual, which the key set leaves open */
static bool
values_of(const Node *q, SiftF2KeySet *ks)
{
	int64		v;

	if (IsA(q, OpExpr) && is_equality(((const OpExpr *) q)->opno) && list_length(((const OpExpr *) q)->args) == 2)
		return int_of(lsecond(((const OpExpr *) q)->args), &v) && add_value(ks, v);
	if (IsA(q, ScalarArrayOpExpr) && ((const ScalarArrayOpExpr *) q)->useOr && is_equality(((const ScalarArrayOpExpr *) q)->opno))
	{
		const Const *arr = lsecond(((const ScalarArrayOpExpr *) q)->args);
		ArrayType  *a;
		int16		len;
		bool		byval;
		char		align;
		Datum	   *el;
		bool	   *nulls;
		int			n;

		if (!IsA(arr, Const) || arr->constisnull || !OidIsValid(get_element_type(arr->consttype)))
			return false;
		a = DatumGetArrayTypeP(arr->constvalue);
		get_typlenbyvalalign(ARR_ELEMTYPE(a), &len, &byval, &align);
		deconstruct_array(a, ARR_ELEMTYPE(a), len, byval, align, &el, &nulls, &n);
		for (int i = 0; i < n; i++)
		{
			const Const k = {.xpr.type = T_Const,.consttype = ARR_ELEMTYPE(a),.constvalue = el[i],.constisnull = nulls[i]};

			if (!int_of((const Node *) &k, &v) || !add_value(ks, v))
				return false;
		}
		return true;
	}
	return false;
}

static int
lead_att_of(Relation index)
{
	const SiftColRel *r;
	const SiftF2Rel *f;

	if (!sift_v6_format()->owns_rel(RelationGetRelid(index), false))
		return 0;
	r = sift_col_rel(index);
	f = sift_f2_rel(index);
	return f->nkey >= 1 ? r->schema->cols[f->key_cols[0]].attno : 0;
}

static bool
gateable(const Node *q, const SiftV6Spec *spec)
{
	SiftV6Spec	scratch = *spec;
	SiftClause	gate;

	return sift_v6_clause_make(CurrentMemoryContext, list_make1((void *) q), &scratch, &gate, NULL);
}

/* the arms differ in quals the gate can evaluate; the pass then needs a quals in common or the values of the lead key to be selective */
static bool
may_union(const SiftV6FuseArm *a, const SiftV6FuseArm *b)
{
	int			common = 0;

	if (a->lead_att == 0 || a->lead_att != b->lead_att)
		return false;
	foreach_ptr(Node, q, a->quals)
		if (list_member(b->quals, q))
			common++;
		else if (!gateable(q, (const SiftV6Spec *) a->spec))
			return false;
	foreach_ptr(Node, q, b->quals)
		if (!list_member(a->quals, q) && !gateable(q, (const SiftV6Spec *) b->spec))
			return false;
	return common > 0 || (a->key_set.n > 0 && b->key_set.n > 0);
}

void
sift_v6_fuse_describe(SiftV6FuseArm *arm, Relation index, const SiftV6Spec *spec)
{
	List	   *lead = NIL;

	arm->spec = (const struct SiftV6Spec *) spec;
	arm->lead_att = lead_att_of(index);
	arm->quals = spec->indexqual;
	arm->may_union = may_union;
	foreach_ptr(Node, q, spec->indexqual)
	{
		Bitmapset  *atts = NULL;

		pull_varattnos(q, INDEX_VAR, &atts);
		if (arm->lead_att > 0 && bms_is_member(arm->lead_att - FirstLowInvalidHeapAttributeNumber, atts))
			lead = lappend(lead, q);
	}
	arm->key_set = (SiftF2KeySet) {0};
	if (list_length(lead) != 1 || !values_of(linitial(lead), &arm->key_set))
		arm->key_set = (SiftF2KeySet) {0};
}

/* the quals every arm of the cohort has: the pass applies them, and no gate does */
static List *
common_quals(const SiftV6FuseCohort *c)
{
	List	   *common = NIL;

	foreach_ptr(Node, q, c->arms[0]->quals)
	{
		bool		all = true;

		for (int i = 1; i < c->narms && all; i++)
			all = list_member(c->arms[i]->quals, q);
		if (all)
			common = lappend(common, q);
	}
	return common;
}

List *
sift_v6_fuse_union_quals(const SiftV6FuseCohort *c, Relation index, List **common)
{
	const SiftV6FuseArm *lead = c->arms[0];
	const SiftF2KeySet *u = &c->union_keys;
	List	   *quals = common_quals(c);
	const Oid	typid = TupleDescAttr(RelationGetDescr(index), lead->lead_att - 1)->atttypid;
	Datum		el[SIFT_F2_MAX_RANGES];
	ArrayType  *arr;
	ScalarArrayOpExpr *sa;
	int16		len;
	bool		byval;
	char		align;
	Oid			opno;

	*common = quals;
	if (u->n == 0)
		return quals;
	for (int i = 0; i < u->n; i++)
	{
		if (u->range[i].ihi != u->range[i].ilo + 1)
			return quals;
		el[i] = typid == INT8OID ? Int64GetDatum(u->range[i].ilo) : typid == INT2OID ? Int16GetDatum((int16) u->range[i].ilo) : Int32GetDatum((int32) u->range[i].ilo);
	}
	get_typlenbyvalalign(typid, &len, &byval, &align);
	arr = construct_array(el, u->n, typid, len, byval, align);
	opno = OpernameGetOprid(list_make1(makeString("=")), typid, typid);
	sa = makeNode(ScalarArrayOpExpr);
	sa->opno = opno;
	sa->opfuncid = get_opcode(opno);
	sa->useOr = true;
	sa->inputcollid = InvalidOid;
	sa->args = list_make2(makeVar(INDEX_VAR, lead->lead_att, typid, -1, InvalidOid, 0),
						  makeConst(get_array_type(typid), -1, InvalidOid, -1, PointerGetDatum(arr), false, false));
	sa->location = -1;
	return lappend(list_copy(quals), sa);
}

bool
sift_v6_fuse_gate(MemoryContext cx, const SiftV6FuseArm *arm, const List *common, const SiftV6Spec *spec, SiftV6Spec **pass_spec, struct SiftClause **gate)
{
	List	   *own = list_difference(arm->quals, common);

	if (own == NIL)
		return false;
	*pass_spec = MemoryContextAlloc(cx, sizeof(SiftV6Spec));
	**pass_spec = *spec;
	*gate = MemoryContextAlloc(cx, sizeof(SiftClause));
	SIFT_INVARIANT(sift_v6_clause_make(cx, own, *pass_spec, *gate, NULL), "the quals of an arm of index %u are not compiled", spec->relid);
	return true;
}
