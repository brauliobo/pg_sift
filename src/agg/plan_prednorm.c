/*
 * agg/plan_prednorm.c - predicates written around a column as tests of the column
 *
 * A condition that wraps its column in coalesce, nullif, a boolean test or an integer widening cast reaches the scan as a generic expression, evaluated for
 * every row. Each of these is the same condition on the column itself, which the comparison kernels run. compile_pred asks for the rewritten condition first
 * and keeps the generic expression when that has no test form. A predicate keeps the rows it is true for, so every form below is exact for "true" (it
 * need not give NULL where the original does); a NOT of them is not rewritten, except NOT of the boolean column and of IS DISTINCT FROM, which never give NULL.
 *   coalesce(a, c) op k   (a op k) OR (a IS NULL AND c op k), c op k folded when both are constants
 *   nullif(a, c) op k     a op k AND a <> c            (c a non-NULL constant: a = c is not true for a NULL a, and a op k is not either)
 *   a IS [NOT] DISTINCT FROM k   a IS NULL OR a <> k;  a = k           (k NULL: a IS NOT NULL; a IS NULL)
 *   b, NOT b, b IS [NOT] TRUE|FALSE|UNKNOWN   b = true, b = false, with b IS NULL where the test is true for NULL
 *   a::int8 op k, a::int4 op k   the cross-type operator of a (int2, int4) and k (int4, int8)
 * Only a column of the table is rewritten (a path of a document keeps its own rules for the rows it reads again).
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"

static Node *
column_of(AggCtx *c, Node *n)
{
	n = strip_relabel(n);
	return IsA(n, Var) && match_leaf(c, n) >= 0 ? n : NULL;
}

static Node *
null_test(Node *a, NullTestType t)
{
	NullTest   *nt = makeNode(NullTest);

	nt->arg = (Expr *) a;
	nt->nulltesttype = t;
	nt->argisrow = false;
	nt->location = -1;
	return (Node *) nt;
}

static Node *
op_on(Oid opno, Node *l, Node *r, Oid collation)
{
	OpExpr	   *o = (OpExpr *) make_opclause(opno, BOOLOID, false, (Expr *) l, (Expr *) r, InvalidOid, collation);

	set_opfuncid(o);
	return (Node *) o;
}

static Node *
op_with(OpExpr *op, int side, Node *x)
{
	OpExpr	   *o = copyObject(op);

	lfirst(list_nth_cell(o->args, side)) = x;
	return (Node *) o;
}

static Node *
bool_eq(Node *b, bool v)
{
	return op_on(BooleanEqualOperator, b, (Node *) makeBoolConst(v, false), InvalidOid);
}

static Node *
either(Node *x, Node *y)
{
	return (Node *) makeBoolExpr(OR_EXPR, list_make2(x, y), -1);
}

static Node *
both(Node *x, Node *y)
{
	return (Node *) makeBoolExpr(AND_EXPR, list_make2(x, y), -1);
}

static int
strategy_of(int cmp)
{
	switch (cmp)
	{
		case SCMP_LT:
			return BTLessStrategyNumber;
		case SCMP_LE:
			return BTLessEqualStrategyNumber;
		case SCMP_GT:
			return BTGreaterStrategyNumber;
		case SCMP_GE:
			return BTGreaterEqualStrategyNumber;
		default:
			return BTEqualStrategyNumber;
	}
}

static bool
widening(Node *n, Node **a)
{
	FuncExpr   *f;

	if (!IsA(n, FuncExpr))
		return false;
	f = (FuncExpr *) n;
	if (f->funcid != F_INT4_INT2 && f->funcid != F_INT8_INT2 && f->funcid != F_INT8_INT4)
		return false;
	*a = strip_relabel(linitial(f->args));
	return true;
}

/* a comparison of an expression of a column with a value the scan knows when it starts */
static Node *
norm_cmp(AggCtx *c, OpExpr *op)
{
	Oid			lt,
				rt;
	int			cmp = builtin_cmp(op->opno, &lt, &rt);

	if (cmp < 0 || list_length(op->args) != 2)
		return NULL;
	for (int side = 0; side < 2; side++)
	{
		Node	   *e = strip_relabel(list_nth(op->args, side)),
				   *k = list_nth(op->args, 1 - side),
				   *a;

		if (!pseudo_const(k))
			continue;
		if (IsA(e, CoalesceExpr) && list_length(((CoalesceExpr *) e)->args) == 2 &&
			(a = column_of(c, linitial(((CoalesceExpr *) e)->args))) != NULL && exprType(a) == exprType(e))
		{
			Node	   *dflt = lsecond(((CoalesceExpr *) e)->args);

			return eval_const_expressions(NULL, either(op_with(op, side, a), both(null_test(a, IS_NULL), op_with(op, side, dflt))));
		}
		if (IsA(e, NullIfExpr) && (a = column_of(c, linitial(((NullIfExpr *) e)->args))) != NULL && exprType(a) == exprType(e) &&
			IsA(lsecond(((NullIfExpr *) e)->args), Const) && !((Const *) lsecond(((NullIfExpr *) e)->args))->constisnull &&
			OidIsValid(get_negator(((NullIfExpr *) e)->opno)))
		{
			NullIfExpr *nx = (NullIfExpr *) e;

			return both(op_with(op, side, a), op_on(get_negator(nx->opno), a, lsecond(nx->args), nx->inputcollid));
		}
		if (widening(e, &a) && column_of(c, a) != NULL)
		{
			Oid			from = exprType(a),
						other = side == 0 ? rt : lt,
						xo = get_opfamily_member(INTEGER_BTREE_FAM_OID, side == 0 ? from : other, side == 0 ? other : from, strategy_of(cmp));

			if (cmp == SCMP_NE && OidIsValid(xo))
				xo = get_negator(xo);
			if (OidIsValid(xo))
				return side == 0 ? op_on(xo, a, k, op->inputcollid) : op_on(xo, k, a, op->inputcollid);
		}
	}
	return NULL;
}

/* `a IS [NOT] DISTINCT FROM k` for a constant k */
static Node *
norm_distinct(AggCtx *c, OpExpr *d, bool negated)
{
	Node	   *a,
			   *k;
	Oid			ne;

	if ((a = column_of(c, linitial(d->args))) != NULL && IsA(strip_relabel(lsecond(d->args)), Const))
		k = strip_relabel(lsecond(d->args));
	else if ((a = column_of(c, lsecond(d->args))) != NULL && IsA(strip_relabel(linitial(d->args)), Const))
		k = strip_relabel(linitial(d->args));
	else
		return NULL;
	if (((Const *) k)->constisnull)
		return null_test(a, negated ? IS_NULL : IS_NOT_NULL);
	if (negated)
		return op_on(d->opno, a, k, d->inputcollid);
	ne = get_negator(d->opno);
	return OidIsValid(ne) ? either(null_test(a, IS_NULL), op_on(ne, a, k, d->inputcollid)) : NULL;
}

Node *
pred_normalize(AggCtx *c, Node *n)
{
	Node	   *b;

	if (IsA(n, OpExpr))
		return norm_cmp(c, (OpExpr *) n);
	if (IsA(n, DistinctExpr))
		return norm_distinct(c, (OpExpr *) n, false);
	if (IsA(n, Var) && exprType(n) == BOOLOID)
		return column_of(c, n) != NULL ? bool_eq(n, true) : NULL;
	if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == NOT_EXPR)
	{
		Node	   *x = strip_relabel(linitial(((BoolExpr *) n)->args));

		if (IsA(x, DistinctExpr))
			return norm_distinct(c, (OpExpr *) x, true);
		return IsA(x, Var) && exprType(x) == BOOLOID && column_of(c, x) != NULL ? bool_eq(x, false) : NULL;
	}
	if (IsA(n, BooleanTest) && (b = column_of(c, (Node *) ((BooleanTest *) n)->arg)) != NULL && exprType(b) == BOOLOID)
	{
		switch (((BooleanTest *) n)->booltesttype)
		{
			case IS_FALSE:
				return bool_eq(b, false);
			case IS_NOT_TRUE:
				return either(bool_eq(b, false), null_test(b, IS_NULL));
			case IS_NOT_FALSE:
				return either(bool_eq(b, true), null_test(b, IS_NULL));
			case IS_UNKNOWN:
				return null_test(b, IS_NULL);
			case IS_NOT_UNKNOWN:
				return null_test(b, IS_NOT_NULL);
			default:
				return NULL;
		}
	}
	return NULL;
}
