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

#include "agg/plan/node_hash.h"
#include "agg/v6/v6_filter_eval.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_vexpr.h"
#include "agg/v6/v6_fuse_keys.h"
#include "col/am.h"
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
	const SiftStoreOps *ops = sift_store_ops_for_oid(RelationGetRelid(index), false);

	return ops != NULL && ops->order != NULL ? ops->order->lead_att(RelationGetRelid(index)) : 0;
}

typedef struct QualEnt
{
	uint64		hash;
	const Node *q;
	int8		gate;			/* whether the gate can evaluate the qual: -1 not asked yet */
} QualEnt;

/* the quals of a list in its order, and the order of their hashes to find one by equal() in a binary search */
typedef struct SiftV6QualSet
{
	int			n;
	int		   *by_hash;
	QualEnt		ent[FLEXIBLE_ARRAY_MEMBER];
} SiftV6QualSet;

static int
qual_cmp(const void *x, const void *y, void *arg)
{
	const QualEnt *ent = arg;
	const uint64 a = ent[*(const int *) x].hash,
				b = ent[*(const int *) y].hash;

	return a < b ? -1 : a > b;
}

static SiftV6QualSet *
qual_set(const List *quals)
{
	const int	n = list_length(quals);
	SiftV6QualSet *s = palloc(offsetof(SiftV6QualSet, ent) + n * sizeof(QualEnt) + n * sizeof(int));
	int			i = 0;

	s->n = n;
	s->by_hash = (int *) &s->ent[n];
	foreach_ptr(Node, q, quals)
	{
		s->ent[i] = (QualEnt) {.hash = sift_node_hash(q),.q = q,.gate = -1};
		s->by_hash[i] = i;
		i++;
	}
	qsort_arg(s->by_hash, n, sizeof(int), qual_cmp, s->ent);
	return s;
}

static bool
qual_has(const SiftV6QualSet *s, const QualEnt *e)
{
	int			lo = 0,
				hi = s->n;

	while (lo < hi)
	{
		const int	mid = lo + (hi - lo) / 2;

		if (s->ent[s->by_hash[mid]].hash < e->hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (; lo < s->n && s->ent[s->by_hash[lo]].hash == e->hash; lo++)
		if (equal(s->ent[s->by_hash[lo]].q, e->q))
			return true;
	return false;
}

static bool
qual_gateable(QualEnt *e, const SiftV6Spec *spec)
{
	if (e->gate < 0)
	{
		SiftV6Spec	scratch = *spec;
		SiftClause	gate;

		e->gate = sift_v6_clause_make(CurrentMemoryContext, list_make1((void *) e->q), &scratch, &gate, NULL);
	}
	return e->gate;
}

/* every qual of a is one of b or one the gate of a can evaluate; common: how many are of b */
static bool
only_gateable(const SiftV6FuseArm *a, const SiftV6FuseArm *b, int *common)
{
	SiftV6QualSet *as = a->qset;

	for (int i = 0; i < as->n; i++)
		if (qual_has(b->qset, &as->ent[i]))
			(*common)++;
		else if (!qual_gateable(&as->ent[i], (const SiftV6Spec *) a->spec))
			return false;
	return true;
}

/* the arms differ in quals the gate can evaluate; the pass then needs a quals in common or the values of the lead key to be selective */
static bool
may_union(const SiftV6FuseArm *a, const SiftV6FuseArm *b)
{
	int			common = 0,
				none = 0;

	if (a->lead_att == 0 || a->lead_att != b->lead_att)
		return false;
	if (!only_gateable(a, b, &common) || !only_gateable(b, a, &none))
		return false;
	return common > 0 || (a->key_set.n > 0 && b->key_set.n > 0);
}

static int
key_range_cmp(const void *x, const void *y)
{
	const int64 a = ((const SiftF2KeyRange *) x)->ilo,
				b = ((const SiftF2KeyRange *) y)->ilo;

	return a < b ? -1 : a > b;
}

void
sift_v6_fuse_describe(SiftV6FuseArm *arm, Relation index, const SiftV6Spec *spec)
{
	List	   *lead = NIL;

	arm->spec = (const struct SiftV6Spec *) spec;
	arm->lead_att = lead_att_of(index);
	arm->quals = spec->indexqual;
	arm->qset = qual_set(spec->indexqual);
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
	qsort(arm->key_set.range, arm->key_set.n, sizeof(SiftF2KeyRange), key_range_cmp);
}

/* the quals every arm of the cohort has: the pass applies them, and no gate does */
static List *
common_quals(const SiftV6FuseCohort *c)
{
	const SiftV6QualSet *first = c->arms[0]->qset;
	List	   *common = NIL;

	for (int e = 0; e < first->n; e++)
	{
		bool		all = true;

		for (int i = 1; i < c->narms && all; i++)
			all = qual_has(c->arms[i]->qset, &first->ent[e]);
		if (all)
			common = lappend(common, (void *) first->ent[e].q);
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
	const SiftV6QualSet *shared = qual_set(common);
	List	   *own = NIL;

	for (int i = 0; i < arm->qset->n; i++)
		if (!qual_has(shared, &arm->qset->ent[i]))
			own = lappend(own, (void *) arm->qset->ent[i].q);
	if (own == NIL)
		return false;
	*pass_spec = MemoryContextAlloc(cx, sizeof(SiftV6Spec));
	**pass_spec = *spec;
	*gate = MemoryContextAlloc(cx, sizeof(SiftClause));
	SIFT_INVARIANT(sift_v6_clause_make(cx, own, *pass_spec, *gate, NULL), "the quals of an arm of index %u are not compiled", spec->relid);
	return true;
}
