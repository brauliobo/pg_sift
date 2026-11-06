/*
 * agg/flat/having.c - a HAVING condition `key IN (sub-select)` or EXISTS of one, that reads no aggregate, is a condition of the rows
 * `GROUP BY k HAVING k IN (SELECT ..)` keeps the groups whose key is in the list: every row of a group has that key, so the rows that are not in it can be dropped before they are
 * grouped, and the sub-select is a semi-join of the rows, which the scan answers, where a HAVING evaluates it once for each group above the aggregate. PostgreSQL
 * moves a HAVING condition to WHERE under the same conditions (subquery_planner: a plain GROUP BY, no aggregate, nothing volatile) except for those with a sub-select, which it keeps
 * as they are for fear of a sub-plan evaluated for each row: the sub-select is a join of the rows when the planner has pulled it up, and the rows form is the plan (row_condition, below,
 * says which conditions: the form on the groups reads more in every one measured). Grouping sets stay: a key is NULL in the rows of a super-aggregate, which the
 * condition is evaluated on, not on the rows. A HAVING with no GROUP BY stays: its one group is there when no row is, a WHERE that drops every row gives no row to the aggregates
 * but still one group.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

/*
 * a sub-select the planner pulls up as a semi join: no aggregate, grouping, window function, set returning function, set operation, CTE or limit (a limit makes it a sub-plan tested
 * for every row, which a HAVING tests for every group)
 */
static bool
semi_subselect(Query *q)
{
	return !q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs && q->groupClause == NIL && q->havingQual == NULL && q->limitCount == NULL && q->limitOffset == NULL &&
		q->setOperations == NULL && q->cteList == NIL;
}

/*
 * a condition of the rows of a group that the scan answers as a semi join: `column = ANY (sub-select)` (IN) with the column as it is (or a relabel of it that keeps its collation:
 * any other cast of it is a function of the key that the groups may not agree on, and stays) and a sub-select that does not read the query, or EXISTS of one, reading no aggregate of
 * this query (one in a sub-select of it counts) and nothing volatile. The rows form reads less than the groups form in every cell measured (1.3 to 10 times), so the planner takes it
 * without a comparison; NOT IN, NOT EXISTS and an expression of the key stay on the groups, where the rows form tests every row (20% and 49% slower)
 */
static bool
row_condition(Node *c)
{
	SubLink    *sl = (SubLink *) c;
	OpExpr	   *op;
	Node	   *key;

	if (!IsA(c, SubLink) || !semi_subselect((Query *) sl->subselect) || contain_aggs_of_level(c, 0) || contain_volatile_functions(c))
		return false;
	if (sl->subLinkType == EXISTS_SUBLINK)
		return true;
	op = (OpExpr *) sl->testexpr;
	if (sl->subLinkType != ANY_SUBLINK || !IsA(op, OpExpr) || contain_vars_of_level(sl->subselect, 1))
		return false;
	key = linitial(op->args);
	if (!op_mergejoinable(op->opno, exprType(key)))
		return false;
	if (IsA(key, RelabelType) && ((RelabelType *) key)->resultcollid == exprCollation((Node *) ((RelabelType *) key)->arg))
		key = (Node *) ((RelabelType *) key)->arg;
	return IsA(key, Var);
}

static bool
plain_group_by(Query *q)
{
	return q->commandType == CMD_SELECT && q->havingQual != NULL && q->hasSubLinks && q->groupClause != NIL && q->groupingSets == NIL && q->jointree != NULL;
}

bool
sift_agg_having_candidate(Query *q)
{
	if (!plain_group_by(q))
		return false;
	foreach_ptr(Node, c, make_ands_implicit((Expr *) q->havingQual))
		if (row_condition(c))
			return true;
	return false;
}

bool
sift_agg_having_to_where(Query *q, bool apply)
{
	List	   *keep = NIL,
			   *rows = NIL;

	if (!sift_agg_having_candidate(q))
		return false;
	if (!apply)
		return true;
	foreach_ptr(Node, c, make_ands_implicit((Expr *) q->havingQual))
	{
		if (row_condition(c))
			rows = lappend(rows, flat_ungroup_expr(q, c));
		else
			keep = lappend(keep, c);
	}
	q->havingQual = keep == NIL ? NULL : (Node *) make_ands_explicit(keep);
	q->jointree->quals = (Node *) make_ands_explicit(list_concat(make_ands_implicit((Expr *) q->jointree->quals), rows));
	return true;
}
