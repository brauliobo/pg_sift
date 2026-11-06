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
 *  - count(CASE WHEN p THEN x ELSE k END), k not NULL, counts every row where p is not true (they give k) and those where p holds and x is not NULL:
 *    count(x) FILTER (WHERE p) + count(*) FILTER (WHERE p IS NOT TRUE).
 *  - min and max with ELSE k, x an integer, a float4 or a float8 and k a constant of the type of x (a zero, or a value): k is one of the values when a row does not hold p, so
 *    CASE WHEN count(*) FILTER (WHERE p IS NOT TRUE) > 0 THEN least(min(x) FILTER (WHERE p), k) ELSE min(x) FILTER (WHERE p) END (greatest and max alike; least and greatest skip the
 *    NULL of a group that has no x and order a NaN as min and max do). Not a numeric: two equal values of a numeric that differ in scale (0.00 and 0) tie, and which one the aggregate
 *    returns depends on the order of the rows. A float tie is a tie of bits, but for the sign of a zero, which a leaf does not keep either.
 *  - avg of an integer or a numeric x with ELSE k, k a whole number (the zeros count as rows): CASE WHEN n > 0 THEN (coalesce(sum(x) FILTER (WHERE p), 0)::numeric + k * count(*) FILTER (WHERE p IS NOT TRUE)) / n::numeric END with
 *    n = count(x) FILTER (WHERE p) + count(*) FILTER (WHERE p IS NOT TRUE), the same numeric_div of the same sum and count as the final function of avg (NULL when n is 0); a whole k adds no scale.
 *    avg of a float8 x with ELSE 0: the same with the sum of float8 and a float8 division: avg adds in float8 as sum does, and a zero changes no sum (k other than 0 does: k * n is no sum of k).
 *    Not a float4: avg(float4) adds in float8 where sum(float4) adds in float4. For int2 and int4 the sum is a bigint, as the state of avg is: both
 *    leave the range of a bigint only after more than 2^32 rows of the largest values. *
 * A condition or a value that holds a sub-select, a volatile function or a set-returning function stays as written, and so do DISTINCT, ORDER BY and the aggregates of other functions.
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/transam.h"
#include "catalog/namespace.h"
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
#include "utils/numeric.h"

#include "agg/agg.h"

bool		sift_agg_case_filter_enabled = true;

typedef enum { CF_SKIP, CF_VALUE, CF_COUNT } CaseElse;

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

/* an integer, or a numeric of scale 0: adding it to a sum of values changes no scale */
static bool
whole(Const *k)
{
	if (k->consttype == NUMERICOID)
		return !numeric_is_nan(DatumGetNumeric(k->constvalue)) && !numeric_is_inf(DatumGetNumeric(k->constvalue)) &&
			DatumGetInt32(DirectFunctionCall1(numeric_scale, k->constvalue)) == 0;
	return k->consttype == INT2OID || k->consttype == INT4OID || k->consttype == INT8OID;
}

/* the aggregates whose result with ELSE k is made of the aggregates of the rows that hold the condition and the number of rows that do not (the header) */
static bool
else_exact(const char *name, Aggref *a, Node *value, Const *k)
{
	Oid			t = exprType(value);
	bool		integer = t == INT2OID || t == INT4OID || t == INT8OID;

	if (k->consttype != t)
		return false;
	if (strcmp(name, "sum") == 0)
		return is_zero(k) && (a->aggtype == INT8OID || a->aggtype == NUMERICOID || a->aggtype == FLOAT4OID || a->aggtype == FLOAT8OID);
	if (strcmp(name, "avg") == 0)
		return ((integer || t == NUMERICOID) && whole(k)) || (t == FLOAT8OID && is_zero(k));
	if (strcmp(name, "min") != 0 && strcmp(name, "max") != 0)
		return false;
	if (t == FLOAT4OID || t == FLOAT8OID)
	{
		double		d = t == FLOAT4OID ? DatumGetFloat4(k->constvalue) : DatumGetFloat8(k->constvalue);

		return !isnan(d) && !(d == 0 && signbit(d));
	}
	return integer;
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
	else if (dflt != NULL && IsA(dflt, Const) && strcmp(name, "count") == 0)
		*other = CF_COUNT;
	else if (dflt != NULL && IsA(dflt, Const) && else_exact(name, a, (Node *) w->result, (Const *) dflt))
	{
		*other = CF_VALUE;
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

/* a zero of the type: a numeric zero keeps the scale of `other`, a zero constant of the query, when it has one */
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
			return other != NULL && other->consttype == NUMERICOID ? copyObject(other) :
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

/* count(*) FILTER (filter), with the level and the collation of the aggregate a */
static Aggref *
count_rows(Aggref *a, Node *filter)
{
	Aggref	   *n = copyObject(a);

	n->aggfnoid = F_COUNT_;
	n->aggtype = INT8OID;
	n->aggtranstype = InvalidOid;
	n->aggstar = true;
	n->args = NIL;
	n->aggargtypes = NIL;
	n->aggfilter = (Expr *) filter;
	return n;
}

/* the rows no value is made of: those the other conditions of the aggregate drop do not count either, so the count has the same filter on the rows */
static Node *
not_true_filter(Aggref *a, Node *cond)
{
	BooleanTest *not_true = makeNode(BooleanTest);

	not_true->arg = (Expr *) copyObject(cond);
	not_true->booltesttype = IS_NOT_TRUE;
	not_true->location = -1;
	return a->aggfilter != NULL ? (Node *) makeBoolExpr(AND_EXPR, list_make2(copyObject(a->aggfilter), not_true), -1) : (Node *) not_true;
}

static Node *
bigint_op(const char *name, Oid resulttype, Oid type, Node *l, Node *r)
{
	OpExpr	   *op = (OpExpr *) make_opclause(OpernameGetOprid(list_make1(makeString((char *) name)), type, type), resulttype, false, (Expr *) l, (Expr *) r, InvalidOid, InvalidOid);

	set_opfuncid(op);
	return (Node *) op;
}

/* count > 0 */
static Node *
some_rows(Node *count)
{
	Oid			gt = get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, BTGreaterStrategyNumber);
	OpExpr	   *op = (OpExpr *) make_opclause(gt, BOOLOID, false, (Expr *) count, (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, true), InvalidOid, InvalidOid);

	set_opfuncid(op);
	return (Node *) op;
}

static Node *
case_when(Oid type, Node *cond, Node *then, Node *otherwise)
{
	CaseWhen   *w = makeNode(CaseWhen);
	CaseExpr   *ce = makeNode(CaseExpr);

	w->expr = (Expr *) cond;
	w->result = (Expr *) then;
	w->location = -1;
	ce->casetype = type;
	ce->args = list_make1(w);
	ce->defresult = (Expr *) otherwise;
	ce->location = -1;
	return (Node *) ce;
}

/* count(CASE WHEN p THEN x ELSE k END): the rows where p is not true give k, which is not NULL */
static Node *
rewrite_count_else(Aggref *a, Node *cond, Node *value)
{
	Aggref	   *others = count_rows(a, not_true_filter(a, cond)),
			   *held;

	if (IsA(value, Const) && ((Const *) value)->constisnull)
		return (Node *) others;
	if (IsA(value, Const))
		held = count_rows(a, with_filter(a, cond));
	else
	{
		held = copyObject(a);
		held->args = list_make1(makeTargetEntry((Expr *) value, 1, NULL, false));
		held->aggfilter = (Expr *) with_filter(a, cond);
	}
	return bigint_op("+", INT8OID, INT8OID, (Node *) held, (Node *) others);
}

/* min, max and avg of CASE WHEN p THEN x ELSE 0 END, x an integer (avg: or a numeric): the header */
static Node *
rewrite_zero_stat(Aggref *a, const char *name, Node *cond, Node *value, Const *zero)
{
	Oid			t = exprType(value);
	Aggref	   *others = count_rows(a, not_true_filter(a, cond)),
			   *held = copyObject(a);
	Node	   *some = some_rows((Node *) others);

	held->args = list_make1(makeTargetEntry((Expr *) value, 1, NULL, false));
	held->aggfilter = (Expr *) with_filter(a, cond);
	if (strcmp(name, "avg") != 0)
	{
		MinMaxExpr *m = makeNode(MinMaxExpr);

		m->minmaxtype = t;
		m->op = strcmp(name, "min") == 0 ? IS_LEAST : IS_GREATEST;
		m->args = list_make2(copyObject(held), copyObject(zero));
		m->location = -1;
		return case_when(t, some, (Node *) m, (Node *) copyObject(held));
	}
	if (t == FLOAT8OID)
	{
		Aggref	   *sum = copyObject(held),
				   *values = copyObject(held);
		CoalesceExpr *co = makeNode(CoalesceExpr);
		Node	   *n;

		sum->aggfnoid = F_SUM_FLOAT8;
		sum->aggtype = FLOAT8OID;
		sum->aggtranstype = InvalidOid;
		sum->aggargtypes = list_make1_oid(FLOAT8OID);
		values->aggfnoid = F_COUNT_ANY;
		values->aggtype = INT8OID;
		values->aggtranstype = InvalidOid;
		values->aggargtypes = list_make1_oid(FLOAT8OID);
		n = bigint_op("+", INT8OID, INT8OID, (Node *) values, (Node *) copyObject(others));
		co->coalescetype = FLOAT8OID;
		co->args = list_make2(sum, zero_of(FLOAT8OID, zero));
		co->location = -1;
		return case_when(FLOAT8OID, some_rows(n), bigint_op("/", FLOAT8OID, FLOAT8OID, (Node *) co, (Node *) makeFuncExpr(F_FLOAT8_INT8, FLOAT8OID, list_make1(copyObject(n)), InvalidOid, InvalidOid, COERCE_IMPLICIT_CAST)),
						 (Node *) makeNullConst(FLOAT8OID, -1, InvalidOid));
	}
	{
		bool		sum_int8 = t != INT8OID && t != NUMERICOID;		/* sum(int2) and sum(int4) are bigints, those of int8 and numerics numerics */
		Aggref	   *sum = copyObject(held),
				   *values = copyObject(held);
		Oid			sumtype = sum_int8 ? INT8OID : NUMERICOID;
		CoalesceExpr *co = makeNode(CoalesceExpr);
		Node	   *total,
				   *n,
				   *count;

		sum->aggfnoid = t == INT2OID ? F_SUM_INT2 : t == INT4OID ? F_SUM_INT4 : t == INT8OID ? F_SUM_INT8 : F_SUM_NUMERIC;
		sum->aggtype = sumtype;
		sum->aggtranstype = InvalidOid;
		sum->aggargtypes = list_make1_oid(t);
		values->aggfnoid = F_COUNT_ANY;
		values->aggtype = INT8OID;
		values->aggtranstype = InvalidOid;
		values->aggargtypes = list_make1_oid(t);
		n = bigint_op("+", INT8OID, INT8OID, (Node *) values, (Node *) copyObject(others));
		co->coalescetype = sumtype;
		co->args = list_make2(sum, zero_of(sumtype, is_zero(zero) ? zero : NULL));
		co->location = -1;
		total = sum_int8 ? (Node *) makeFuncExpr(F_NUMERIC_INT8, NUMERICOID, list_make1(co), InvalidOid, InvalidOid, COERCE_IMPLICIT_CAST) : (Node *) co;
		if (!is_zero(zero))
		{
			/* every row where p is not true adds k, a whole number: the scale of the sum stays what the values give it */
			Node	   *k = zero->consttype == NUMERICOID ? (Node *) copyObject(zero) :
				(Node *) makeConst(NUMERICOID, -1, InvalidOid, -1, DirectFunctionCall1(int8_numeric, Int64GetDatum(zero->consttype == INT2OID ? DatumGetInt16(zero->constvalue) :
								   zero->consttype == INT4OID ? DatumGetInt32(zero->constvalue) : DatumGetInt64(zero->constvalue))), false, false);
			Node	   *others_n = (Node *) makeFuncExpr(F_NUMERIC_INT8, NUMERICOID, list_make1(copyObject(others)), InvalidOid, InvalidOid, COERCE_IMPLICIT_CAST);

			total = bigint_op("+", NUMERICOID, NUMERICOID, total, bigint_op("*", NUMERICOID, NUMERICOID, k, others_n));
		}
		count = (Node *) makeFuncExpr(F_NUMERIC_INT8, NUMERICOID, list_make1(copyObject(n)), InvalidOid, InvalidOid, COERCE_IMPLICIT_CAST);
		return case_when(NUMERICOID, some_rows(n), bigint_op("/", NUMERICOID, NUMERICOID, total, count), (Node *) makeNullConst(NUMERICOID, -1, InvalidOid));
	}
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
	if (other == CF_COUNT)
		return rewrite_count_else(a, cond, value);
	if (other == CF_VALUE && strcmp(get_func_name(a->aggfnoid), "sum") != 0)
		return rewrite_zero_stat(a, get_func_name(a->aggfnoid), cond, value, dc);
	if (count_of_constant(a, value))
		return (Node *) count_rows(a, with_filter(a, cond));
	f = copyObject(a);
	if (other == CF_SKIP)
	{
		f->args = list_make1(makeTargetEntry((Expr *) value, 1, NULL, false));
		f->aggfilter = (Expr *) with_filter(a, cond);
		return (Node *) f;
	}
	{
		CoalesceExpr *co = makeNode(CoalesceExpr);
		Aggref	   *others = count_rows(a, not_true_filter(a, cond));

		f->args = list_make1(makeTargetEntry((Expr *) value, 1, NULL, false));
		f->aggfilter = (Expr *) with_filter(a, cond);
		co->coalescetype = a->aggtype;
		co->args = list_make2(copyObject(f), zero_of(a->aggtype, dc));
		co->location = -1;
		return case_when(a->aggtype, some_rows((Node *) others), (Node *) co, (Node *) copyObject(f));
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
