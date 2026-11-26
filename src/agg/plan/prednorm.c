/*
 * agg/plan/prednorm.c - the comparison of one column a WHERE clause implies when the clause is no index qual itself: the leading column of a row comparison ((a, b) > (x, y) implies
 * a >= x), the column of a coalesce whose default fails the comparison (coalesce(a, k) < c is a < c when k < c is not true), the column of a nullif under a strict operator
 * (nullif(a, k) = c implies a = c) and a IS NOT DISTINCT FROM of a constant that is not NULL (a = c). The scan prunes and filters by the implied comparison, and the clause still
 * decides every row it keeps.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/plan/int.h"
#include "core/compat.h"

/* (a, b, ..) op (x, y, ..): the leading pair under the operator of its family that also holds on the ties of the leading column */
static Expr *
row_compare_lead(const RowCompareExpr *rc)
{
	const Node *l = linitial(rc->largs),
			   *r = linitial(rc->rargs);
	const int	kind = ROWCMP_KIND(rc),
				strat = kind == ROWCMP_LT || kind == ROWCMP_LE ? BTLessEqualStrategyNumber : kind == ROWCMP_GT || kind == ROWCMP_GE ? BTGreaterEqualStrategyNumber : InvalidStrategy;
	Oid			opno;

	if (strat == InvalidStrategy)
		return NULL;
	opno = get_opfamily_member(linitial_oid(rc->opfamilies), exprType(l), exprType(r), strat);
	return OidIsValid(opno) ? make_opclause(opno, BOOLOID, false, copyObject((Expr *) l), copyObject((Expr *) r), InvalidOid, linitial_oid(rc->inputcollids)) : NULL;
}

/* the value of op applied to the default of a coalesce and the other operand, both constants: what a row whose column is NULL compares as */
static bool
default_passes(PlannerInfo *root, OpExpr *op, Node *dflt, Node *other, bool dflt_left)
{
	OpExpr	   *test = copyObject(op);
	Node	   *v;

	test->args = dflt_left ? list_make2(dflt, other) : list_make2(other, dflt);
	v = estimate_expression_value(root, (Node *) test);
	return !IsA(v, Const) || (!((Const *) v)->constisnull && DatumGetBool(((Const *) v)->constvalue));
}

/* op with its operand at side replaced by n */
static Expr *
with_operand(OpExpr *op, int side, Node *n)
{
	OpExpr	   *q = copyObject(op);

	q->args = side == 0 ? list_make2(copyObject(n), lsecond(q->args)) : list_make2(linitial(q->args), copyObject(n));
	return (Expr *) q;
}

/* coalesce(a, k) op c, k and c constants: a op c when k op c is not true, so that a NULL column fails the clause as it fails a op c */
static Expr *
coalesce_column(PlannerInfo *root, OpExpr *op, int side)
{
	const CoalesceExpr *ce = list_nth(op->args, side);
	Node	   *other = list_nth(op->args, 1 - side);

	if (list_length(ce->args) != 2 || !IsA(linitial(ce->args), Var) || !IsA(lsecond(ce->args), Const) || !IsA(other, Const) ||
		default_passes(root, op, lsecond(ce->args), other, side == 0))
		return NULL;
	return with_operand(op, side, linitial(ce->args));
}

/* nullif(a, k) op c under a strict op: a row that passes has nullif(a, k) = a, so a op c */
static Expr *
nullif_column(OpExpr *op, int side)
{
	const NullIfExpr *ni = list_nth(op->args, side);

	return IsA(linitial(ni->args), Var) && op_strict(op->opno) ? with_operand(op, side, linitial(ni->args)) : NULL;
}

static Expr *
op_implied(PlannerInfo *root, OpExpr *op)
{
	if (list_length(op->args) != 2)
		return NULL;
	for (int side = 0; side < 2; side++)
	{
		const Node *n = list_nth(op->args, side);

		if (IsA(n, CoalesceExpr))
			return coalesce_column(root, op, side);
		if (IsA(n, NullIfExpr))
			return nullif_column(op, side);
	}
	return NULL;
}

/* NOT (a IS DISTINCT FROM c) with c not NULL: a = c */
static Expr *
not_distinct_equal(const BoolExpr *b)
{
	const DistinctExpr *d = linitial(b->args);

	if (b->boolop != NOT_EXPR || !IsA(d, DistinctExpr) || !IsA(lsecond(d->args), Const) || ((const Const *) lsecond(d->args))->constisnull)
		return NULL;
	return make_opclause(d->opno, BOOLOID, false, copyObject((Expr *) linitial(d->args)), copyObject((Expr *) lsecond(d->args)), InvalidOid, d->inputcollid);
}

static Expr *
implied_of(PlannerInfo *root, Expr *clause)
{
	switch (nodeTag(clause))
	{
		case T_RowCompareExpr:
			return row_compare_lead((const RowCompareExpr *) clause);
		case T_OpExpr:
			return op_implied(root, (OpExpr *) clause);
		case T_BoolExpr:
			return not_distinct_equal((const BoolExpr *) clause);
		default:
			return NULL;
	}
}

Expr *
sift_v6_implied_qual(PlannerInfo *root, Expr *clause)
{
	Expr	   *q = implied_of(root, clause);

	if (q != NULL)
		fix_opfuncids((Node *) q);
	return q;
}
