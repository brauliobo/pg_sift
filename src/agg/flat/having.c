/*
 * agg/flat/having.c - a HAVING condition with a sub-select that reads no aggregate is a condition of the rows
 * `GROUP BY k HAVING k IN (SELECT ..)` keeps the groups whose key is in the list: every row of a group has that key, so the rows that are not in it can be dropped before they are
 * grouped, and the sub-select is a semi-join (or anti-join) of the rows, which the scan answers, where a HAVING evaluates it once for each group above the aggregate. PostgreSQL
 * moves a HAVING condition to WHERE under the same conditions (subquery_planner: a plain GROUP BY, no aggregate, nothing volatile) except for those with a sub-select, which it keeps
 * as they are for fear of a sub-plan evaluated for each row: the sub-select is a join of the rows when the planner has pulled it up, and the planner hook plans both forms and keeps the
 * cheaper (so a few groups of many rows over an expensive correlated sub-select stay as written). Grouping sets stay: a key is NULL in the rows of a super-aggregate, which the
 * condition is evaluated on, not on the rows. A HAVING with no GROUP BY stays: its one group is there when no row is, a WHERE that drops every row gives no row to the aggregates
 * but still one group.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteManip.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

/* a condition of the rows of a group: it holds the sub-select, and reads no aggregate of this query (one in a sub-select of it counts), no window function, nothing volatile */
static bool
row_condition(Node *c)
{
	return checkExprHasSubLink(c) && !contain_aggs_of_level(c, 0) && !contain_volatile_functions(c);
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
	sift_agg_by_cost = sift_agg_having_moved = true;
	return true;
}
