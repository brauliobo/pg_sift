/*
 * sift_pushdown.c - bitmap paths for the filter-mask pushdown of the ranked
 * SiftScan (sift_pushdown.h).
 *
 * The planner builds a bitmap scan for a relation's restrictions in
 * create_index_paths, which is not exported in pieces. It is run again over the
 * relation's other indexes with plain index scans disabled, so the bitmap path
 * survives the dominance check of add_path, and the relation's own paths are
 * put back afterwards.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "miscadmin.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/guc.h"

#include "sift.h"
#include "sift_pushdown.h"

int			sift_filter_pushdown = SIFT_PUSHDOWN_ON;

static const struct config_enum_entry pushdown_options[] = {
	{"off", SIFT_PUSHDOWN_OFF, false},
	{"on", SIFT_PUSHDOWN_ON, false},
	{"force", SIFT_PUSHDOWN_FORCE, false},
	{"false", SIFT_PUSHDOWN_OFF, true},
	{"true", SIFT_PUSHDOWN_ON, true},
	{NULL, 0, false}
};

void
sift_pushdown_init(void)
{
	DefineCustomEnumVariable("sift.filter_pushdown",
							 "Ranked scans take the tuples a bitmap of the other quals keeps: off, on (by cost) or force.",
							 NULL, &sift_filter_pushdown, SIFT_PUSHDOWN_ON, pushdown_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);
}

/* The indexes that can answer a bitmap scan and are not sift's */
static List *
bitmap_indexes(RelOptInfo *rel)
{
	List	   *usable = NIL;

	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam != sift_catalog()->am && ii->amhasgetbitmap)
			usable = lappend(usable, ii);
	return usable;
}

BitmapHeapPath *
sift_filter_bitmap_path(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *indexes = NIL,
			   *paths = rel->pathlist,
			   *partial = rel->partial_pathlist,
			   *indexlist = rel->indexlist,
			   *joininfo = rel->joininfo;
	bool		eclass_joins = rel->has_eclass_joins,
				parallel = rel->consider_parallel;
	BitmapHeapPath *best = NULL;
	int			nest;

	if (sift_filter_pushdown == SIFT_PUSHDOWN_OFF || !enable_bitmapscan || rel->lateral_relids != NULL ||
		(indexes = bitmap_indexes(rel)) == NIL)
		return NULL;
	rel->pathlist = rel->partial_pathlist = rel->joininfo = NIL;
	rel->indexlist = indexes;
	rel->has_eclass_joins = rel->consider_parallel = false;
	nest = NewGUCNestLevel();
	set_config_option("enable_indexscan", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	set_config_option("enable_indexonlyscan", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	create_index_paths(root, rel);
	AtEOXact_GUC(true, nest);
	foreach_node(Path, p, rel->pathlist)
		if (IsA(p, BitmapHeapPath) && p->param_info == NULL &&
			(best == NULL || p->total_cost < best->path.total_cost))
			best = (BitmapHeapPath *) p;
	rel->pathlist = paths;
	rel->partial_pathlist = partial;
	rel->indexlist = indexlist;
	rel->joininfo = joininfo;
	rel->has_eclass_joins = eclass_joins;
	rel->consider_parallel = parallel;
	return best;
}

void
sift_filter_estimate(PlannerInfo *root, RelOptInfo *rel, BitmapHeapPath *bp, SiftFilterEst *est)
{
	double		tuples,
				pages,
				maxentries,
				lossy;
	Cost		index_cost;

	pages = compute_bitmap_pages(root, rel, bp->bitmapqual, 1.0, &index_cost, &tuples);
	maxentries = tbm_calculate_entries((Size) work_mem * 1024);
	lossy = pages > maxentries ? 1.0 - maxentries / pages : 0.0;
	est->sel = Min(tuples / Max(rel->tuples, 1), 1.0);
	est->sel += (Min(pages / Max(rel->pages, 1), 1.0) - est->sel) * lossy;
	est->cost = index_cost + tuples * cpu_operator_cost;
}

void
sift_filter_plan_deps(PlannerInfo *root, Plan *bitmap)
{
	if (IsA(bitmap, BitmapIndexScan))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, ((BitmapIndexScan *) bitmap)->indexid);
	else if (IsA(bitmap, BitmapAnd))
		foreach_ptr(Plan, p, ((BitmapAnd *) bitmap)->bitmapplans)
			sift_filter_plan_deps(root, p);
	else if (IsA(bitmap, BitmapOr))
		foreach_ptr(Plan, p, ((BitmapOr *) bitmap)->bitmapplans)
			sift_filter_plan_deps(root, p);
}
