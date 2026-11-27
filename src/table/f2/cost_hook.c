/*
 * table/f2/cost_hook.c - the index paths of a sift_table: before PostgreSQL 18 no bitmap heap scan (its executor prefetches the virtual blocks of the TIDs from the file, core/compat.h), and the
 * price of the rows an index path of a sift_table of Format 2.0 fetches (f2_cost_hook.h). It is added to the cost the access method of the index estimates, so that every
 * path built on the index (an index scan, a bitmap index scan, a parameterized scan of a nested loop) carries it when the planner compares it with the others, and none is thrown away as dearer than a
 * path it was priced too cheaply against.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "utils/rel.h"

#include "core/catalog.h"
#include "table/dispatch.h"
#include "table/f2/cost_hook.h"

/* Before 19 the routine of an access method is a copy the caller owns; from 19 it is the one the server keeps */
static void
release_am_routine(const IndexAmRoutine *am)
{
#if PG_VERSION_NUM < 190000
	pfree((void *) am);
#endif
}

static void
f2_index_costestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages)
{
	IndexOptInfo *index = path->indexinfo;
	const IndexAmRoutine *am = GetIndexAmRoutineByAmId(index->relam, false);

	am->amcostestimate(root, path, loop_count, startup, total, selectivity, correlation, pages);
	if (path->path.pathtype != T_IndexOnlyScan)
		*total += clamp_row_est(*selectivity * index->rel->tuples) * SIFT_TBL_F2_FETCH_ROW_US / SIFT_TBL_COST_UNIT_US;
	release_am_routine(am);
}

/* The index paths of a sift_table: no bitmap heap scan where the executor prefetches the virtual blocks of its TIDs from the file, and the price of the rows a Format 2.0 table fetches */
static void
adjust_index_paths(Oid relid, RelOptInfo *rel)
{
	if (!sift_relid_is_sift_table(relid))
		return;
	foreach_node(IndexOptInfo, index, rel->indexlist)
	{
		if (SIFT_BITMAP_EXECUTOR_PREFETCH)
			index->amhasgetbitmap = false;
		if (sift_tbl_is_f2_oid(relid))
			index->amcostestimate = f2_index_costestimate;
	}
}

#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
f2_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	adjust_index_paths(relid, rel);
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
		adjust_index_paths(rte->relid, rel);
}

void
sift_tbl_f2_cost_hook_init(void)
{
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = f2_build_simple_rel;
}
#endif
