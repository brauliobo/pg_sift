/*
 * agg/flat/constmul.c - a sum of a column times a value that is the same for every row is the sum of the column times that value
 * `sum(v * (SELECT count(*) FROM kinds))` and `sum(v * $1)` (a generic plan) multiply each row by a value the statement asks once: the product is a numeric expression of a column and
 * a parameter, which the scan does not compute (its numeric programs hold constants known when the plan is made). The sum of the products is the product of the sum: numeric
 * multiplication is exact, the display scale of the sum of products is the largest scale of the column plus that of the factor, as that of the product of the sum, and NULL, NaN
 * and the infinities come out the same (a NULL factor makes every product NULL, and the sum of nothing is NULL). The aggregate is the sum of the column, which the scan computes, and the
 * product is made above it, once for each group.
 */
#include "postgres.h"

#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

/* a sub-select or a parameter of the statement: a value the statement asks once, not a constant the planner folds */
static bool
asked_once_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubLink) && ((SubLink *) n)->subLinkType == EXPR_SUBLINK)
		return true;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXTERN)
		return true;
	return expression_tree_walker(n, asked_once_walker, context);
}

/* the same for every row of the query: no column, no aggregate, nothing volatile, no sub-select that reads a column of the query */
static bool
same_for_every_row(Node *n)
{
	return asked_once_walker(n, NULL) && !contain_var_clause(n) && !contain_aggs_of_level(n, 0) && !contain_volatile_functions(n) && !contain_vars_of_level(n, 0);
}

typedef struct Factor
{
	bool		apply;
	bool		changed;
} Factor;

static Node *
factor_mutator(Node *n, Factor *f)
{
	Aggref	   *a;
	TargetEntry *te;
	OpExpr	   *m;
	Node	   *l,
			   *r,
			   *column;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Aggref))
		return expression_tree_mutator(n, factor_mutator, f);
	a = (Aggref *) n;
	if (a->aggfnoid != F_SUM_NUMERIC || a->agglevelsup != 0 || a->aggdistinct != NIL || a->aggorder != NIL || a->aggkind != AGGKIND_NORMAL || list_length(a->args) != 1)
		return n;
	te = linitial_node(TargetEntry, a->args);
	if (!IsA(te->expr, OpExpr))
		return n;
	m = (OpExpr *) te->expr;
	set_opfuncid(m);
	if (m->opfuncid != F_NUMERIC_MUL || list_length(m->args) != 2)
		return n;
	l = linitial(m->args);
	r = lsecond(m->args);
	if (same_for_every_row(r) && contain_var_clause(l) && exprType(l) == NUMERICOID)
		column = l;
	else if (same_for_every_row(l) && contain_var_clause(r) && exprType(r) == NUMERICOID)
		column = r;
	else
		return n;
	f->changed = true;
	if (!f->apply)
		return n;
	{
		Aggref	   *sum = copyObject(a);
		OpExpr	   *product = copyObject(m);
		TargetEntry *ste = linitial_node(TargetEntry, sum->args);

		ste->expr = (Expr *) copyObject(column);
		product->args = column == l ? list_make2(sum, copyObject(r)) : list_make2(copyObject(l), sum);
		return (Node *) product;
	}
}

/* a sum of numerics: the read-only test that the statements with no such sum (nearly all) pass before the mutator copies their trees */
static bool
sum_numeric_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		return ((Aggref *) n)->aggfnoid == F_SUM_NUMERIC;
	return expression_tree_walker(n, sum_numeric_walker, context);
}

bool
sift_agg_factor_constants(Query *q, bool apply)
{
	Factor		f = {apply, false};

	if (q->commandType != CMD_SELECT || !q->hasAggs || (!sum_numeric_walker((Node *) q->targetList, NULL) && !sum_numeric_walker(q->havingQual, NULL)))
		return false;
	foreach_node(TargetEntry, te, q->targetList)
		te->expr = (Expr *) factor_mutator((Node *) te->expr, &f);
	q->havingQual = factor_mutator(q->havingQual, &f);
	if (f.changed && apply)
		sift_agg_by_cost = sift_agg_flat_any = true;
	return f.changed;
}
