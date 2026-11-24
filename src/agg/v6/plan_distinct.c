/*
 * agg/v6/plan_distinct.c - SELECT DISTINCT of the columns of one table at the UPPERREL_DISTINCT stage: a group-by of the DISTINCT list with no aggregate.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/v6/plan_int.h"
#include "agg/v6/v6_vexpr.h"

/* a DISTINCT over a grouped, windowed or aggregated query is above that stage's own paths: not the scan's */
static bool
distinct_of_plain_scan(const Query *q)
{
	return q->distinctClause != NIL && !q->hasDistinctOn && !q->hasAggs && q->groupClause == NIL && q->groupingSets == NIL && q->havingQual == NULL;
}

void
sift_v6_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const Query *q = root->parse;
	List	   *keys = NIL;

	if (!distinct_of_plain_scan(q) || root->upper_targets[UPPERREL_DISTINCT] == NULL)
		return;
	sift_v6_stage_target(root, UPPERREL_DISTINCT, output_rel);
	if (!sift_v6_plan_shape(root, input_rel, output_rel))
		return;
	foreach_node(SortGroupClause, sgc, q->distinctClause)
		keys = lappend(keys, get_sortgroupclause_expr(sgc, q->targetList));
	sift_v6_scan_paths(root, input_rel, output_rel, NULL, keys);
}
