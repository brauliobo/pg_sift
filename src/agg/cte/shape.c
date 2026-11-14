/*
 * agg/cte/shape.c - what a statement holds that the rewrites of the hook look for
 * Each rewrite candidate (a shared CTE, an object built by json_build_object, an aggregate of a CASE, an EXISTS, an ARRAY sub-link, a correlated
 * min() or max(), a lateral sub-select with a sub-link, a left join) needs one kind of node in the tree to find anything. One walk records which kinds
 * there are, and a candidate whose node is not there is not walked for.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "utils/fmgroids.h"

#include "agg/agg.h"
#include "agg/cte/int.h"

static bool
shape_walker(Node *n, QueryShape *s)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	switch (nodeTag(n))
	{
		case T_Query:
			s->ctes |= ((Query *) n)->cteList != NIL;
			foreach_node(RangeTblEntry, rte, ((Query *) n)->rtable)
				s->lateral_sublinks |= rte->rtekind == RTE_SUBQUERY && rte->lateral && rte->subquery->hasSubLinks;
			return query_tree_walker((Query *) n, shape_walker, s, 0);
		case T_FuncExpr:
			s->json |= ((FuncExpr *) n)->funcid == F_JSON_BUILD_OBJECT_ANY;
			break;
		case T_Aggref:
			s->case_aggs |= list_length(((Aggref *) n)->args) == 1 && IsA(linitial_node(TargetEntry, ((Aggref *) n)->args)->expr, CaseExpr);
			break;
		case T_SubLink:
			s->exists |= ((SubLink *) n)->subLinkType == EXISTS_SUBLINK;
			s->array |= ((SubLink *) n)->subLinkType == ARRAY_SUBLINK;
			s->minmax |= ((SubLink *) n)->subLinkType == EXPR_SUBLINK && castNode(Query, ((SubLink *) n)->subselect)->hasAggs;
			break;
		case T_JoinExpr:
			s->left_joins |= ((JoinExpr *) n)->jointype == JOIN_LEFT;
			break;
		default:
			break;
	}
	return expression_tree_walker(n, shape_walker, s);
}

QueryShape
sift_agg_shape(Query *q)
{
	QueryShape	s = {0};

	shape_walker((Node *) q, &s);
	return s;
}
