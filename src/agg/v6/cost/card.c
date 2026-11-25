/*
 * agg/v6/cost/card.c - the clauses of a relation as the predicates of agg/v6/cost/card_est.h, and the rows and groups the statistics of the index give for them.
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/v6/safety.h"
#include "agg/v6/v6.h"
#include "agg/v6/cost/card.h"
#include "agg/v6/vexpr.h"

/* the integer a Const stands for in the order of its leaf: int2, int4, int8, date, timestamp, timestamptz; false for any other type */
static bool
leaf_value(Oid typid, Datum d, int64 *out)
{
	switch (typid)
	{
		case INT2OID:
			*out = DatumGetInt16(d);
			return true;
		case INT4OID:
		case DATEOID:
			*out = DatumGetInt32(d);
			return true;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			*out = DatumGetInt64(d);
			return true;
		default:
			return false;
	}
}

int
sift_v6_card_leaf(const IndexOptInfo *ix, int col)
{
	return sift_v6_rel_card_leaf(ix->indexoid, col);
}

static int
leaf_of(const IndexOptInfo *ix, Index relid, const Node *e)
{
	const Var  *v = (const Var *) e;
	SiftV6Class cls;

	if (e == NULL || !IsA(e, Var) || v->varno != relid || v->varlevelsup != 0)
		return -1;
	cls = sift_v6_class(v->vartype);
	return (cls == SV6C_I32 || cls == SV6C_I64) && v->vartype != NUMERICOID && agg_index_col(ix, v->varattno) >= 0 ? sift_v6_card_leaf(ix, agg_index_col(ix, v->varattno)) : -1;
}

static SiftCardPred *
pred_add(SiftV6CardQuery *o, SiftCardPredKind kind, int leaf)
{
	SiftCardPred *p;

	if (o->q.npreds == SIFT_CARD_MAX_PREDS)
		return NULL;
	p = &o->preds[o->q.npreds++];
	*p = (SiftCardPred) {.kind = kind,.col = (uint16) leaf};
	return p;
}

static void
range_add(SiftV6CardQuery *o, int leaf, bool is_lo, int64 bound)
{
	SiftCardPred *p = NULL;

	for (uint32 i = 0; i < o->q.npreds && p == NULL; i++)
		if (o->preds[i].kind == SCP_RANGE && o->preds[i].col == leaf)
			p = &o->preds[i];
	if (p == NULL && (p = pred_add(o, SCP_RANGE, leaf)) == NULL)
		return;
	if (is_lo)
	{
		p->lo = p->has_lo ? Max(p->lo, bound) : bound;
		p->has_lo = true;
	}
	else
	{
		p->hi = p->has_hi ? Min(p->hi, bound) : bound;
		p->has_hi = true;
	}
}

static bool
compare_pred(SiftV6CardQuery *o, const char *name, int leaf, int64 c, bool flipped)
{
	SiftCardPred *p;
	const char *op = name;

	if (flipped)
		op = name[0] == '<' ? (name[1] == '=' ? ">=" : ">") : name[0] == '>' ? (name[1] == '=' ? "<=" : "<") : name;
	if (strcmp(op, "=") == 0)
	{
		if ((p = pred_add(o, SCP_EQ, leaf)) == NULL)
			return false;
		o->vals[o->nvals] = (uint64) c;
		p->vals = &o->vals[o->nvals++];
		p->nvals = 1;
		return true;
	}
	if (strcmp(op, "<") == 0 && c > INT64_MIN)
		range_add(o, leaf, false, c - 1);
	else if (strcmp(op, "<=") == 0)
		range_add(o, leaf, false, c);
	else if (strcmp(op, ">") == 0 && c < INT64_MAX)
		range_add(o, leaf, true, c + 1);
	else if (strcmp(op, ">=") == 0)
		range_add(o, leaf, true, c);
	else
		return false;
	return true;
}

static bool
op_pred(SiftV6CardQuery *o, const OpExpr *op, Index relid, const IndexOptInfo *ix)
{
	const Node *l,
			   *r;
	const char *name;
	int			leaf;
	int64		c;
	bool		flipped;

	if (list_length(op->args) != 2 || get_func_namespace(get_opcode(op->opno)) != PG_CATALOG_NAMESPACE || (name = get_opname(op->opno)) == NULL)
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	flipped = IsA(r, Var);
	if (flipped)
	{
		const Node *t = l;

		l = r;
		r = t;
	}
	if ((leaf = leaf_of(ix, relid, l)) < 0 || !IsA(r, Const) || ((Const *) r)->constisnull || !leaf_value(((Const *) r)->consttype, ((Const *) r)->constvalue, &c))
		return false;
	return compare_pred(o, name, leaf, c, flipped);
}

/* col = ANY($1): the one clause with a parameter the estimator takes, as a list of a length it does not know; every other use of a parameter has no value at plan time */
static bool
is_param_list(const Node *e)
{
	return IsA(e, ScalarArrayOpExpr) && IsA(lsecond(((ScalarArrayOpExpr *) e)->args), Param) && !agg_node_has_extern_param(linitial(((ScalarArrayOpExpr *) e)->args));
}

static bool
in_pred(SiftV6CardQuery *o, const ScalarArrayOpExpr *sa, Index relid, const IndexOptInfo *ix)
{
	const Node *arr;
	const char *name;
	SiftCardPred *p;
	int			leaf;

	if (!sa->useOr || list_length(sa->args) != 2 || (leaf = leaf_of(ix, relid, linitial(sa->args))) < 0 || (name = get_opname(sa->opno)) == NULL || strcmp(name, "=") != 0)
		return false;
	arr = lsecond(sa->args);
	if (IsA(arr, Param) && ((Param *) arr)->paramkind == PARAM_EXTERN)
	{
		if ((p = pred_add(o, SCP_IN, leaf)) != NULL)
			p->unknown_len = true;
		return p != NULL;
	}
	if (IsA(arr, Const) && !((Const *) arr)->constisnull)
	{
		ArrayType  *a = DatumGetArrayTypeP(((Const *) arr)->constvalue);
		Datum	   *elems;
		bool	   *nulls;
		int			n;
		const uint32 first = o->nvals;
		int64		c;

		if (ARR_NDIM(a) > 1 || (uint64) o->nvals + ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a)) > o->cap)
			return false;
		sift_v6_array_elems(a, &elems, &nulls, &n);
		if ((p = pred_add(o, SCP_IN, leaf)) == NULL)
			return false;
		for (int i = 0; i < n; i++)
			if (!nulls[i] && leaf_value(ARR_ELEMTYPE(a), elems[i], &c))
				o->vals[o->nvals++] = (uint64) c;
		p->vals = &o->vals[first];
		p->nvals = o->nvals - first;
		return true;
	}
	return false;
}

static bool
null_pred(SiftV6CardQuery *o, const NullTest *nt, Index relid, const IndexOptInfo *ix)
{
	const int	leaf = nt->argisrow ? -1 : leaf_of(ix, relid, (Node *) nt->arg);

	return leaf >= 0 && pred_add(o, nt->nulltesttype == IS_NULL ? SCP_IS_NULL : SCP_IS_NOT_NULL, leaf) != NULL;
}

static uint32
const_array_len(const Node *e)
{
	const ArrayType *a;

	if (e == NULL || !IsA(e, Const) || ((Const *) e)->constisnull)
		return 0;
	a = DatumGetArrayTypeP(((Const *) e)->constvalue);
	return ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
}

/* every equality and every constant list of the clauses fit one array of values: a list holds at most the elements of its array, and the array of values at most what one allocation holds (a list that does not fit is left to the executor) */
static uint32
values_bound(const List *clauses)
{
	uint64		n = list_length(clauses);

	foreach_ptr(RestrictInfo, ri, (List *) clauses)
		if (IsA(ri->clause, ScalarArrayOpExpr))
			n += const_array_len(lsecond(((ScalarArrayOpExpr *) ri->clause)->args));
	return (uint32) Min(n, MaxAllocSize / sizeof(uint64));
}

void
sift_v6_card_query(const List *clauses, Index relid, const IndexOptInfo *ix, SiftV6CardQuery *out)
{
	*out = (SiftV6CardQuery) {0};
	out->cap = values_bound(clauses);
	out->vals = palloc(Max(out->cap, 1) * sizeof(uint64));
	foreach_ptr(RestrictInfo, ri, (List *) clauses)
	{
		const Node *e = (Node *) ri->clause;
		bool		taken;

		if (agg_node_has_extern_param(e) && !is_param_list(e))
		{
			out->rest = lappend(out->rest, ri);
			continue;
		}
		switch (nodeTag(e))
		{
			case T_OpExpr:
				taken = op_pred(out, (OpExpr *) e, relid, ix);
				break;
			case T_ScalarArrayOpExpr:
				taken = in_pred(out, (ScalarArrayOpExpr *) e, relid, ix);
				break;
			case T_NullTest:
				taken = null_pred(out, (NullTest *) e, relid, ix);
				break;
			default:
				taken = false;
		}
		if (!taken)
			out->rest = lappend(out->rest, ri);
	}
	out->q = (SiftCardQuery) {.preds = out->preds,.npreds = out->q.npreds,.lead_col = -1,.has_uncovered = out->rest != NIL};
}

static bool
may_leak(PlannerInfo *root, RelOptInfo *rel)
{
	return root->parse->hasRowSecurity || agg_rte_has_rls(root, planner_rt_fetch(rel->relid, root));
}

bool
sift_v6_card_rel(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view)
{
	SiftV6CardQuery cq;
	SiftCardEst est;
	double		rest_sel = 1.0;

	if (view == NULL || view->rows_from_pg || may_leak(root, rel))
		return false;
	sift_v6_card_query(rel->baserestrictinfo, rel->relid, ix, &cq);
	sift_card_estimate(view, &cq.q, &est);
	if (est.confidence < SIFT_V6_CARD_TRUST)
		return false;
	if (cq.rest != NIL)
		rest_sel = clauselist_selectivity(root, cq.rest, 0, JOIN_INNER, NULL);
	rel->rows = clamp_row_est(est.est_rows * rest_sel);
	return true;
}

double
sift_v6_card_prune(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, List **rest)
{
	SiftV6CardQuery cq;
	SiftCardEst est;

	if (view == NULL || view->n_sample == 0 || may_leak(root, rel))
	{
		*rest = rel->baserestrictinfo;
		return -1;
	}
	sift_v6_card_query(rel->baserestrictinfo, rel->relid, ix, &cq);
	sift_card_estimate(view, &cq.q, &est);
	*rest = cq.rest;
	return est.g_total > 0 ? est.g_eff / est.g_total : -1;
}

double
sift_v6_card_groups(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, const List *group_exprs, double rows)
{
	double		d[SIFT_CARD_MAX_PREDS];
	int			n = 0;

	if (view == NULL || may_leak(root, rel) || list_length(group_exprs) > SIFT_CARD_MAX_PREDS)
		return -1;
	foreach_ptr(Node, e, (List *) group_exprs)
	{
		const int	leaf = leaf_of(ix, rel->relid, e);

		if (leaf < 0 || (uint32) leaf >= view->ncols || !(view->cols[leaf].flags & SIFT_CSTAT_HAS_NDV))
			return -1;
		d[n++] = sift_card_ndv(view->cols[leaf].ndv_est, Max((double) view->total_live_rows, 1.0), rows);
	}
	return n == 0 ? 1.0 : sift_card_ndv_multi(d, n, rows, 0);
}
