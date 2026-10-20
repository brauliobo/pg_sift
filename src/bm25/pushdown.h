/*
 * bm25/pushdown.h - planner side of the filter-mask pushdown (extras
 * item 10): a ranked SiftScan whose child is a bitmap scan over the other
 * quals of the relation; the executor turns the bitmap into masks the ranker
 * ANDs into its candidates (sift_filter.h).
 */
#ifndef SIFT_PUSHDOWN_H
#define SIFT_PUSHDOWN_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

typedef enum SiftPushdown
{
	SIFT_PUSHDOWN_OFF,
	SIFT_PUSHDOWN_ON,			/* when the planner's estimate says it is cheaper */
	SIFT_PUSHDOWN_FORCE			/* whenever a bitmap path exists */
} SiftPushdown;

extern PGDLLIMPORT int sift_filter_pushdown;

/* What the mask costs and keeps */
typedef struct SiftFilterEst
{
	Cost		cost;			/* the bitmap scan and the masks */
	double		sel;			/* fraction of the heap's tuples the masks keep (lossy pages keep all of theirs) */
	double		start;			/* fraction of the heap before the first tuple the bitmap can keep, in scan order */
	double		span;			/* fraction of the heap from that tuple to the last one the bitmap can keep */
} SiftFilterEst;

/*
 * The bitmap path over the indexes other than sift's that serves the relation's restrictions best,
 * as the planner would build it for a bitmap heap scan; NULL when pushdown is off or no index
 * serves them. Leaves the relation's path list as it was.
 */
extern BitmapHeapPath *sift_filter_bitmap_path(PlannerInfo *root, RelOptInfo *rel);

/*
 * entries: how many of the bitmap's tuples, in heap order, a scan that checks the filter after the
 * ranker has to meet before it holds as many rows that pass as its limit (0: not known). The heap
 * position of those entries is read from the btree when the filter is an equality on its one column.
 */
extern void sift_filter_estimate(PlannerInfo *root, RelOptInfo *rel, BitmapHeapPath *bp, double entries, SiftFilterEst *est);

/* Records the indexes of the bitmap plan as dependencies of the plan */
extern void sift_filter_plan_deps(PlannerInfo *root, Plan *bitmap);

extern void sift_pushdown_init(void);

#endif							/* SIFT_PUSHDOWN_H */
