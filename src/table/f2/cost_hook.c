/*
 * table/f2/cost_hook.c - the price of the rows an index path of a sift_table of Format 2.0 fetches (f2_cost_hook.h). It is added to the cost the access method of the index estimates, so that every
 * path built on the index (an index scan, a bitmap index scan, a parameterized scan of a nested loop) carries it when the planner compares it with the others, and none is thrown away as dearer than a
 * path it was priced too cheaply against.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "utils/rel.h"

#include "table/dispatch.h"
#include "table/f2/cost_hook.h"

static void
f2_index_costestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages)
{
	IndexOptInfo *index = path->indexinfo;
	IndexAmRoutine *am = GetIndexAmRoutineByAmId(index->relam, false);

	am->amcostestimate(root, path, loop_count, startup, total, selectivity, correlation, pages);
	if (path->path.pathtype != T_IndexOnlyScan)
		*total += clamp_row_est(*selectivity * index->rel->tuples) * SIFT_TBL_F2_FETCH_ROW_US / SIFT_TBL_COST_UNIT_US;
	pfree(am);
}

static void
price_fetches(Oid relid, RelOptInfo *rel)
{
	if (!sift_tbl_is_f2_oid(relid))
		return;
	foreach_node(IndexOptInfo, index, rel->indexlist)
		index->amcostestimate = f2_index_costestimate;
}

#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
f2_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	price_fetches(relid, rel);
}

void
sift_tbl_f2_cost_hook_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = f2_get_relation_info;
}
#else
static build_simple_rel_hook_type prev_simple_rel_hook;

static void
f2_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_simple_rel_hook)
		prev_simple_rel_hook(root, rel, rte);
	if (rte->rtekind == RTE_RELATION)
		price_fetches(rte->relid, rel);
}

void
sift_tbl_f2_cost_hook_init(void)
{
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = f2_build_simple_rel;
}
#endif
