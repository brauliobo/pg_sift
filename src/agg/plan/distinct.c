/*
 * agg/plan/distinct.c - SELECT DISTINCT of the columns of one table, of its partitions, or of the join of a fact table to its dimensions, at the UPPERREL_DISTINCT stage: a group-by of the
 * DISTINCT list with no aggregate.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"

#include "agg/join/plan.h"
#include "agg/plan/int.h"
#include "agg/vexpr.h"

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
	foreach_node(SortGroupClause, sgc, q->distinctClause)
		keys = lappend(keys, get_sortgroupclause_expr(sgc, q->targetList));
	if (input_rel->reloptkind == RELOPT_JOINREL)
	{
		sift_v6_lookup_paths(root, input_rel, output_rel, NULL, keys);
	}
	else if (!sift_v6_plan_shape(root, input_rel, output_rel))
		return;
	else if (input_rel->part_scheme != NULL && planner_rt_fetch(input_rel->relid, root)->inh)
		sift_agg_v6_partition_paths(root, input_rel, output_rel, NULL, keys);
	else
		sift_v6_scan_paths(root, input_rel, output_rel, NULL, keys);
}
