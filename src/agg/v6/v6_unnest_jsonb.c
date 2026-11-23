/*
 * agg/v6/v6_unnest_jsonb.c - the lateral jsonb_to_recordset of the aggregate scan of an unnest (agg/v6/v6_unnest_jsonb.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, and the elements of one row of the table */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#include "agg/group/ht.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "agg/vexpr_str.h"
#include "col/am.h"

/* the type of an element column the scan holds as a vector, and the leaf type that holds it */
static bool
col_holds(Oid typid, int32 typmod, const SiftColDef *d)
{
	if (typmod != -1 || d->kind != SIFT_COLK_SCALAR || d->lane != 0 || (d->flags & (SIFT_COLD_LOSSY | SIFT_COLD_ASTEXT)))
		return false;
	switch (typid)
	{
		case TEXTOID:
		case VARCHAROID:
			return d->store == SIFT_COLS_STR && d->type == SIFT_COLT_TEXT;
		case INT2OID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_INT2;
		case INT4OID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_INT4;
		case INT8OID:
			return d->store == SIFT_COLS_INT && d->type == SIFT_COLT_INT8;
		case FLOAT8OID:
			return d->store == SIFT_COLS_F8 && d->type == SIFT_COLT_FLOAT8;
		default:
			return false;
	}
}

bool
sift_v6_unnest_rte(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *rel, SiftV6ElemRte *out)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	const RangeTblFunction *rtf;
	const FuncExpr *fe;
	const Var  *v;

	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 || rel->joininfo != NIL ||
		!bms_equal(rel->lateral_relids, bms_make_singleton(fact->relid)))
		return false;
	rtf = linitial(rte->functions);
	fe = (const FuncExpr *) rtf->funcexpr;
	if (!IsA(rtf->funcexpr, FuncExpr) || list_length(fe->args) != 1 || !IsA(linitial(fe->args), Var))
		return false;
	v = linitial(fe->args);
	if (v->varno != fact->relid || v->varlevelsup != 0)
		return false;
	*out = (SiftV6ElemRte) {.array = v};
	if (fe->funcid == F_UNNEST_ANYARRAY)
	{
		out->fn = SV6E_UNNEST;
		out->elem = get_element_type(v->vartype);
		return rtf->funccolcount == 1 && fe->funcresulttype == out->elem &&
			(out->elem == TEXTOID || out->elem == VARCHAROID || out->elem == INT2OID || out->elem == INT4OID || out->elem == INT8OID);
	}
	if (fe->funcid != F_JSONB_TO_RECORDSET || v->vartype != JSONBOID || rtf->funccolcount < 1 || rtf->funccolcount > SIFT_V6_XELEM_COLS)
		return false;
	out->fn = SV6E_RECORDSET;
	out->names = rtf->funccolnames;
	for (int c = 0; c < rtf->funccolcount; c++)
	{
		const Oid	t = list_nth_oid(rtf->funccoltypes, c);

		if (list_nth_int(rtf->funccoltypmods, c) != -1)
			return false;
		out->types = lappend_oid(out->types, t);
	}
	return true;
}

/* the lengths leaf of the attribute and its element leaves are the ones of a list of objects (no key leaf, which a map has) */
static int
group_of(const SiftColRel *r, int att)
{
	int			group = -1;

	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].kind == SIFT_COLK_GROUP)
			group = i;
	return group;
}

/* the leaf of the key `name` of the objects of the group */
static int
member_leaf(const SiftColRel *r, int att, int group, const char *name)
{
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
	{
		const SiftColDef *d = &r->schema->cols[i];
		const char *dot = d->path != NULL ? strrchr(d->path, '.') : NULL;

		if (d->parent == group && d->path != NULL && strcmp(dot != NULL ? dot + 1 : d->path, name) == 0 && strchr(name, '#') == NULL)
			return i;
	}
	return -1;
}

static bool
seed_recordset(V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, int att, List **names)
{
	const SiftColRel *r = p->colrel;
	int			c = 0;

	if (!r->json[att] || (e->group_leaf = group_of(r, att)) < 0)
		return false;
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].parent == e->group_leaf && r->schema->cols[i].path != NULL && strstr(r->schema->cols[i].path, "#key") != NULL)
			return false;
	foreach_node(String, n, rte->names)
	{
		const int	leaf = member_leaf(r, att, e->group_leaf, strVal(n));
		const Oid	t = list_nth_oid(rte->types, c);

		if (leaf < 0 || !col_holds(t, -1, &r->schema->cols[leaf]))
			return false;
		e->col_leaf[c] = leaf;
		e->col_type[c++] = t;
		*names = lappend(*names, makeString(pstrdup(strVal(n))));
	}
	e->ncols = c;
	return true;
}

/* the lengths leaf of the array of attribute att and the leaf of its elements: the one of unnest */
static bool
seed_unnest_array(V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, int att)
{
	const SiftColRel *r = p->colrel;
	const SiftV6Class cls = sift_v6_class(rte->elem);

	e->group_leaf = e->col_leaf[0] = -1;
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].kind == SIFT_COLK_GROUP)
			e->group_leaf = i;
	for (int i = r->first[att]; e->group_leaf >= 0 && i < r->first[att] + r->nleaves[att]; i++)
	{
		const SiftColDef *d = &r->schema->cols[i];

		if (d->parent == e->group_leaf && d->kind == SIFT_COLK_SCALAR && d->lane == 0 && !(d->flags & SIFT_COLD_LOSSY) && (cls == SV6C_TEXT ? d->store == SIFT_COLS_STR : d->store == SIFT_COLS_INT))
			e->col_leaf[0] = i;
	}
	e->col_type[0] = rte->elem;
	e->ncols = 1;
	return e->col_leaf[0] >= 0;
}

bool
sift_v6_unnest_seed(V6Plan *p, RelOptInfo *elem_rel, const SiftV6ElemRte *rte, bool left)
{
	SiftV6Ext  *x = sift_v6_ext_new();
	List	   *names = NIL;

	for (int i = 0; i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == rte->array->varattno)
		{
			x->unnest = true;
			x->elem = (SiftV6XElem) {.fn = rte->fn,.attno = i + 1,.type = rte->fn == SV6E_UNNEST ? rte->elem : rte->array->vartype,.left = left};
			if (!(rte->fn == SV6E_RECORDSET ? seed_recordset(p, &x->elem, rte, i + 1, &names) : seed_unnest_array(p, &x->elem, rte, i + 1)))
				return false;
			x->elem_names = names;
			p->xseed = x;
			p->elem_rel = elem_rel;
			return true;
		}
	return false;
}

bool
sift_v6_unnest_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	const SiftV6Ext *x = p->xseed;
	SiftV6Class cls;

	if (p->elem_rel == NULL || x == NULL || !IsA(e, Var) || v->varno != p->elem_rel->relid || v->varlevelsup != 0 || v->varattno < 1 || v->varattno > x->elem.ncols)
		return false;
	cls = sift_v6_class(v->vartype);
	if (cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_TEXT && v->vartype != FLOAT8OID)
		return false;
	if (sift_v6_class(x->elem.col_type[v->varattno - 1]) != cls || (cls == SV6C_I32 && x->elem.col_type[v->varattno - 1] != v->vartype))
		return false;
	*out = (SiftV6Col) {.attno = x->elem.attno,.typid = v->vartype,.src = SV6SRC_ELEM,.slot = v->varattno - 1};
	return true;
}

/* date_trunc('month', date column)::date: the month of the date, which PostgreSQL finds through timestamptz and the zone of the session, is a key the batches of the scan compute (v6_unnest_run.c) */
bool
sift_v6_unnest_month_key(V6Plan *p, const Expr *e, SiftV6Col *k)
{
	const FuncExpr *out = (const FuncExpr *) e;
	const FuncExpr *trunc,
			   *cast;
	const Const *unit;

	if (p->xseed == NULL || !IsA(e, FuncExpr) || out->funcid != F_DATE_TIMESTAMPTZ || !IsA(linitial(out->args), FuncExpr))
		return false;
	trunc = linitial(out->args);
	if (trunc->funcid != F_DATE_TRUNC_TEXT_TIMESTAMPTZ || list_length(trunc->args) != 2 || !IsA(linitial(trunc->args), Const) || !IsA(lsecond(trunc->args), FuncExpr))
		return false;
	unit = linitial(trunc->args);
	cast = lsecond(trunc->args);
	return !unit->constisnull && pg_strcasecmp(TextDatumGetCString(unit->constvalue), "month") == 0 && cast->funcid == F_TIMESTAMPTZ_DATE &&
		sift_v6_plan_column(p, linitial(cast->args), false, k) && k->typid == DATEOID;
}

/* ---- the quals of the element ---- */

static Expr *
elem_var(const Var *v)
{
	return (Expr *) makeVar(INDEX_VAR, SIFT_V6_ELEM_VATT + v->varattno - 1, v->vartype, v->vartypmod, v->varcollid, 0);
}

static const Var *
elem_column(const V6Plan *p, const Node *n)
{
	const Var  *v = (const Var *) n;
	SiftV6Col	col;

	return n != NULL && IsA(n, Var) && sift_v6_unnest_column((V6Plan *) p, (const Expr *) n, &col) ? v : NULL;
}

/* a comparison of a column of the element with a constant, an IN list of constants, or a null test, over the virtual attribute of the column */
static Expr *
elem_qual(const V6Plan *p, Node *clause)
{
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *q = copyObject((OpExpr *) clause);
		Node	   *l = linitial(q->args),
				   *r = lsecond(q->args);

		if (IsA(r, Var) && IsA(l, Const))
		{
			q->opno = get_commutator(q->opno);
			if (!OidIsValid(q->opno))
				return NULL;
			q->opfuncid = get_opcode(q->opno);
			l = lsecond(q->args);
			r = linitial(q->args);
		}
		if (elem_column(p, l) == NULL || !IsA(r, Const) || (sift_v6_class(exprType(l)) == SV6C_TEXT && !sift_text_bytes_equal(q->inputcollid)))
			return NULL;
		q->args = list_make2(elem_var((Var *) l), r);
		return (Expr *) q;
	}
	if (IsA(clause, ScalarArrayOpExpr) && list_length(((ScalarArrayOpExpr *) clause)->args) == 2)
	{
		ScalarArrayOpExpr *q = copyObject((ScalarArrayOpExpr *) clause);

		if (elem_column(p, linitial(q->args)) == NULL || !IsA(lsecond(q->args), Const) || (sift_v6_class(exprType(linitial(q->args))) == SV6C_TEXT && !sift_text_bytes_equal(q->inputcollid)))
			return NULL;
		linitial(q->args) = elem_var(linitial(q->args));
		return (Expr *) q;
	}
	if (IsA(clause, NullTest) && elem_column(p, (Node *) ((NullTest *) clause)->arg) != NULL && !((NullTest *) clause)->argisrow)
	{
		NullTest   *q = copyObject((NullTest *) clause);

		q->arg = elem_var((Var *) q->arg);
		return (Expr *) q;
	}
	return NULL;
}

bool
sift_v6_unnest_elem_quals(V6Plan *p)
{
	SiftClause *clause = palloc(sizeof(SiftClause));
	int			col[SIFT_V6_MAX_FCOLS],
				nvec;
	List	   *quals = NIL;

	foreach_node(RestrictInfo, ri, p->elem_rel->baserestrictinfo)
	{
		Expr	   *q = ri->pseudoconstant ? NULL : elem_qual(p, (Node *) ri->clause);

		if (q == NULL)
			return false;
		quals = lappend(quals, q);
	}
	if (quals != NIL && (p->xseed->elem.left || !sift_v6_unnest_elem_clause(CurrentMemoryContext, quals, clause, col, &nvec)))
		return false;
	p->xseed->elem_where = quals;
	return true;
}

bool
sift_v6_unnest_elem_clause(MemoryContext cx, const List *quals, SiftClause *out, int *col, int *nvec)
{
	SiftV6Spec	tmp = {0};

	if (!sift_v6_clause_make(cx, quals, &tmp, out, NULL))
		return false;
	for (int i = 0; i < tmp.nfcols; i++)
		col[i] = tmp.fcol[i].attno - SIFT_V6_ELEM_VATT;
	*nvec = tmp.nfcols;
	return true;
}

/* ---- the document of a row the leaves cannot hold ---- */

struct SiftV6RecFn
{
	FmgrInfo	flinfo;
	ReturnSetInfo rsinfo;
	ExprContext *econtext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	int			ncols;
	Oid			type[SIFT_V6_XELEM_COLS];
};

SiftV6RecFn *
sift_v6_recfn_new(MemoryContext cx, const SiftV6Ext *x)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV6RecFn *f = palloc0(sizeof(SiftV6RecFn));
	int			c = 0;

	f->ncols = x->elem.ncols;
	f->desc = CreateTemplateTupleDesc(f->ncols);
	foreach_node(String, n, x->elem_names)
	{
		TupleDescInitEntry(f->desc, c + 1, strVal(n), x->elem.col_type[c], -1, 0);
		TupleDescInitEntryCollation(f->desc, c + 1, sift_v6_class(x->elem.col_type[c]) == SV6C_TEXT ? DEFAULT_COLLATION_OID : InvalidOid);
		f->type[c] = x->elem.col_type[c];
		c++;
	}
	sift_tupdesc_finalize(f->desc);
	BlessTupleDesc(f->desc);
	fmgr_info(F_JSONB_TO_RECORDSET, &f->flinfo);
	f->econtext = CreateStandaloneExprContext();
	f->slot = MakeSingleTupleTableSlot(f->desc, &TTSOpsMinimalTuple);
	f->rsinfo = (ReturnSetInfo) {.type = T_ReturnSetInfo,.econtext = f->econtext,.expectedDesc = f->desc,.allowedModes = (int) SFRM_Materialize,.returnMode = SFRM_ValuePerCall};
	MemoryContextSwitchTo(old);
	return f;
}

static void
put_cell(MemoryContext cx, SiftVec *v, Oid type, uint32 k, Datum d, bool isnull)
{
	if (isnull)
		return;
	v->valid[k >> 6] |= UINT64CONST(1) << (k & 63);
	switch (type)
	{
		case TEXTOID:
		case VARCHAROID:
			{
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
				const uint32 len = (uint32) VARSIZE_ANY_EXHDR(t);
				char	   *copy = MemoryContextAlloc(cx, Max(len, 1));

				memcpy(copy, VARDATA_ANY(t), len);
				((SiftStrView *) v->data)[k] = sift_vstr_make(copy, len);
				break;
			}
		case FLOAT8OID:
			((double *) v->data)[k] = DatumGetFloat8(d);
			break;
		case INT2OID:
			((int64 *) v->data)[k] = DatumGetInt16(d);
			break;
		case INT4OID:
			((int64 *) v->data)[k] = DatumGetInt32(d);
			break;
		default:
			((int64 *) v->data)[k] = DatumGetInt64(d);
	}
}

uint32
sift_v6_recfn_call(SiftV6RecFn *f, Datum doc, MemoryContext cx, SiftVec *ev)
{
	LOCAL_FCINFO(fcinfo, 1);
	Tuplestorestate *ts;
	uint32		n;

	f->rsinfo.setResult = NULL;
	InitFunctionCallInfoData(*fcinfo, &f->flinfo, 1, InvalidOid, NULL, (Node *) &f->rsinfo);
	fcinfo->args[0].value = doc;
	fcinfo->args[0].isnull = false;
	FunctionCallInvoke(fcinfo);
	ts = f->rsinfo.setResult;
	n = ts == NULL ? 0 : (uint32) tuplestore_tuple_count(ts);
	for (int c = 0; c < f->ncols; c++)
	{
		ev[c].type = sift_v6_class(f->type[c]) == SV6C_F64 ? SVEC_F64 : sift_v6_class(f->type[c]) == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
		ev[c].data = MemoryContextAllocAligned(cx, (Size) Max(n, 1) * sift_gather_width(ev[c].type) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		ev[c].valid = MemoryContextAllocZero(cx, ((n + 63) / 64 + 1) * sizeof(uint64));
		ev[c].all_valid = false;
		ev[c].len = (uint16) Min(n, UINT16_MAX);
	}
	for (uint32 k = 0; ts != NULL && tuplestore_gettupleslot(ts, true, false, f->slot); k++)
	{
		slot_getallattrs(f->slot);
		for (int c = 0; c < f->ncols; c++)
			put_cell(cx, &ev[c], f->type[c], k, f->slot->tts_values[c], f->slot->tts_isnull[c]);
	}
	if (ts != NULL)
		tuplestore_end(ts);
	return n;
}
