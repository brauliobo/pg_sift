/*
 * agg/expr/casefilter.c - agg(CASE WHEN p THEN x [ELSE NULL | 0] END) as agg(x) FILTER (WHERE p)
 *
 * A pivot of sums over a CASE hands the scan an expression of two leaves for each aggregate, evaluated for every row on the generic path. The same aggregate with
 * a filter runs on the kernels of x under a test of p. The rewrite is exact:
 *  - ELSE NULL (or none) and sum, avg, min, max, count: the aggregates skip NULL, a row where p does not hold gives NULL, so it is a row the filter drops;
 *  - count(CASE WHEN p THEN <constant> END) is count(*) FILTER (WHERE p);
 *  - sum(CASE WHEN p THEN x ELSE 0 END) adds a zero for every row where p does not hold: the sum is NULL only when no row at all holds x, which is
 *    CASE WHEN count(*) FILTER (WHERE p IS NOT TRUE) > 0 THEN coalesce(sum(x) FILTER (WHERE p), 0) ELSE sum(x) FILTER (WHERE p) END
 *    (a group where p never holds is 0, an empty input is NULL, rows that hold a NULL x and no other row give NULL: all as the CASE form).
 * avg and min/max with ELSE 0 are not the same aggregate (the zeros count) and stay. A condition or a value that holds a sub-select, a volatile function or a
 * set-returning function stays as written, and so do DISTINCT, ORDER BY and the aggregates of other functions.
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/transam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"

bool		sift_agg_case_filter_enabled = true;

typedef enum { CF_NONE, CF_SKIP, CF_ZERO } CaseElse;

static Node *
test_value(Node *n, Node *arg)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, CaseTestExpr))
		return copyObject(arg);
	return expression_tree_mutator(n, test_value, arg);
}

static bool
has_nested(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc))
		return true;
	return expression_tree_walker(n, has_nested, context);
}

static bool
plain_expr(Node *n)
{
	return !contain_volatile_functions(n) && !expression_returns_set(n) && !has_nested(n, NULL);
}

static bool
is_zero(Const *c)
{
	if (c->constisnull)
		return false;
	switch (c->consttype)
	{
		case INT2OID:
			return DatumGetInt16(c->constvalue) == 0;
		case INT4OID:
			return DatumGetInt32(c->constvalue) == 0;
		case INT8OID:
			return DatumGetInt64(c->constvalue) == 0;
		case FLOAT4OID:
			return DatumGetFloat4(c->constvalue) == 0 && !signbit(DatumGetFloat4(c->constvalue));
		case FLOAT8OID:
			return DatumGetFloat8(c->constvalue) == 0 && !signbit(DatumGetFloat8(c->constvalue));
		case NUMERICOID:
			/* a zero of scale 2 makes the sum of the CASE form show two decimals, the sum of the values alone does not */
			return DatumGetBool(DirectFunctionCall2(numeric_eq, c->constvalue, DirectFunctionCall1(int4_numeric, Int32GetDatum(0)))) &&
				DatumGetInt32(DirectFunctionCall1(numeric_scale, c->constvalue)) == 0;
		default:
			return false;
	}
}

/* the shape of the aggregate: the condition, the value and what the other branch gives; false when it is not one of the shapes above */
static bool
case_arm(Aggref *a, Node **cond, Node **value, CaseElse *other, Const **zero)
{
	CaseExpr   *ce;
	CaseWhen   *w;
	Node	   *dflt;
	const char *name;

	if (a->aggkind != AGGKIND_NORMAL || a->aggstar || a->aggorder != NIL || a->aggdistinct != NIL || a->aggdirectargs != NIL || list_length(a->args) != 1 ||
		a->aggfnoid >= FirstNormalObjectId || !IsA(linitial_node(TargetEntry, a->args)->expr, CaseExpr))
		return false;
	name = get_func_name(a->aggfnoid);
	if (name == NULL || (strcmp(name, "sum") != 0 && strcmp(name, "avg") != 0 && strcmp(name, "min") != 0 && strcmp(name, "max") != 0 && strcmp(name, "count") != 0))
		return false;
	ce = (CaseExpr *) linitial_node(TargetEntry, a->args)->expr;
	if (list_length(ce->args) != 1)
		return false;
	w = linitial_node(CaseWhen, ce->args);
	/* the other branch as the planner folds it: a constant of another type is a call of its cast until then */
	dflt = (Node *) ce->defresult;
	if (dflt != NULL && !IsA(dflt, Const))
		dflt = plain_expr(dflt) ? eval_const_expressions(NULL, copyObject(dflt)) : NULL;
	*zero = NULL;
	if (ce->defresult == NULL || (dflt != NULL && IsA(dflt, Const) && ((Const *) dflt)->constisnull))
		*other = CF_SKIP;
	else if (dflt != NULL && IsA(dflt, Const) && strcmp(name, "sum") == 0 && is_zero((Const *) dflt) &&
			 (a->aggtype == INT8OID || a->aggtype == NUMERICOID || a->aggtype == FLOAT4OID || a->aggtype == FLOAT8OID))
	{
		*other = CF_ZERO;
		*zero = (Const *) dflt;
	}
	else
		return false;
	if (ce->arg != NULL && !IsA(ce->arg, Var) && !IsA(ce->arg, Const))
		return false;
	*cond = ce->arg != NULL ? test_value((Node *) w->expr, (Node *) ce->arg) : (Node *) w->expr;
	*value = (Node *) w->result;
	return plain_expr(*cond) && plain_expr(*value);
}

static bool
count_of_constant(Aggref *a, Node *value)
{
	return a->aggfnoid == F_COUNT_ANY && IsA(value, Const) && !((Const *) value)->constisnull;
}

static Const *
zero_of(Oid type, Const *other)
{
	switch (type)
	{
		case INT8OID:
			return makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true);
		case FLOAT4OID:
			return makeConst(FLOAT4OID, -1, InvalidOid, sizeof(float4), Float4GetDatum(0), false, true);
		case FLOAT8OID:
			return makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8), Float8GetDatum(0), false, FLOAT8PASSBYVAL);
		default:
			return other->consttype == NUMERICOID ? copyObject(other) :
				makeConst(NUMERICOID, -1, InvalidOid, -1, DirectFunctionCall1(int8_numeric, Int64GetDatum(0)), false, false);
	}
}

static Node *
with_filter(Aggref *a, Node *cond)
{
	if (a->aggfilter != NULL)
		cond = (Node *) makeBoolExpr(AND_EXPR, list_make2(a->aggfilter, cond), -1);
	return cond;
}

static Node *
rewrite_aggref(Aggref *a)
{
	Node	   *cond,
			   *value;
	CaseElse	other;
	Const	   *dc;
	Aggref	   *f;

	if (!case_arm(a, &cond, &value, &other, &dc))
		return NULL;
	f = copyObject(a);
	if (count_of_constant(a, value))
	{
		f->aggfnoid = F_COUNT_;
		f->aggtype = INT8OID;
		f->aggtranstype = InvalidOid;
		f->aggstar = true;
		f->args = NIL;
		f->aggargtypes = NIL;
		f->aggfilter = (Expr *) with_filter(a, cond);
		return (Node *) f;
	}
	if (other == CF_SKIP)
	{
		f->args = list_make1(makeTargetEntry((Expr *) value, 1, NULL, false));
		f->aggfilter = (Expr *) with_filter(a, cond);
		return (Node *) f;
	}
	{
		Aggref	   *n = copyObject(a);
		Const	   *zero = zero_of(a->aggtype, dc);
		Oid			gt = get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, BTGreaterStrategyNumber);
		BooleanTest *not_true = makeNode(BooleanTest);
		CoalesceExpr *co = makeNode(CoalesceExpr);
		CaseWhen   *w = makeNode(CaseWhen);
		CaseExpr   *ce = makeNode(CaseExpr);
		OpExpr	   *op;

		f->args = list_make1(makeTargetEntry((Expr *) value, 1, NULL, false));
		f->aggfilter = (Expr *) with_filter(a, cond);
		/* the rows no sum is made of: those the other conditions of the aggregate drop do not count either, so the count has the same filter on the rows */
		n->aggfnoid = F_COUNT_;
		n->aggtype = INT8OID;
		n->aggtranstype = InvalidOid;
		n->aggstar = true;
		n->args = NIL;
		n->aggargtypes = NIL;
		not_true->arg = (Expr *) copyObject(cond);
		not_true->booltesttype = IS_NOT_TRUE;
		not_true->location = -1;
		n->aggfilter = a->aggfilter != NULL ? (Expr *) makeBoolExpr(AND_EXPR, list_make2(copyObject(a->aggfilter), not_true), -1) : (Expr *) not_true;
		co->coalescetype = a->aggtype;
		co->args = list_make2(copyObject(f), zero);
		co->location = -1;
		op = (OpExpr *) make_opclause(gt, BOOLOID, false, (Expr *) n, (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true), InvalidOid, InvalidOid);
		set_opfuncid(op);
		w->expr = (Expr *) op;
		w->result = (Expr *) co;
		w->location = -1;
		ce->casetype = a->aggtype;
		ce->args = list_make1(w);
		ce->defresult = (Expr *) copyObject(f);
		ce->location = -1;
		return (Node *) ce;
	}
}

static Node *
case_filter_mutator(Node *n, void *context)
{
	Node	   *r;

	if (n == NULL)
		return NULL;
	SIFT_SKIP_CONST_LIST(n, n);
	if (IsA(n, Query))
		return (Node *) query_tree_mutator((Query *) n, case_filter_mutator, context, QTW_DONT_COPY_QUERY);
	if (IsA(n, Aggref) && (r = rewrite_aggref((Aggref *) n)) != NULL)
		return r;
	return expression_tree_mutator(n, case_filter_mutator, context);
}

static bool
case_filter_walker(Node *n, void *context)
{
	Node	   *cond,
			   *value;
	CaseElse	other;
	Const	   *dc;

	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, case_filter_walker, context, 0);
	if (IsA(n, Aggref) && case_arm((Aggref *) n, &cond, &value, &other, &dc))
		return true;
	return expression_tree_walker(n, case_filter_walker, context);
}

/* an aggregate is in the level itself (hasAggs) or in one below it, which a sub-link, a CTE or a sub-select leads to: a statement with none of them is not walked */
static bool
may_hold_aggs(Query *q)
{
	if (q->hasAggs || q->hasSubLinks || q->cteList != NIL)
		return true;
	foreach_node(RangeTblEntry, r, q->rtable)
		if (r->rtekind == RTE_SUBQUERY)
			return true;
	return false;
}

/* does a query tree hold an aggregate of that shape? A walk that builds nothing, asked before the tree is copied for the rewrite */
bool
sift_agg_case_filter_calls(Query *q)
{
	return sift_agg_case_filter_enabled && may_hold_aggs(q) && case_filter_walker((Node *) q, NULL);
}

void
sift_agg_case_filter(Query *q)
{
	query_tree_mutator(q, case_filter_mutator, NULL, QTW_DONT_COPY_QUERY);
}
