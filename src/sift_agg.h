/*
 * sift_agg.h - SiftAgg: aggregate queries run on the column source (sift_colsrc.h) instead of the heap.
 *
 * sift_aggplan.c is the planner side (shape matcher in create_upper_paths_hook, cost), sift_agg.c the executor
 * (Custom Scan (SiftAgg)), sift_aggkern*.c the vectorized operators. The query's result is the one of the plain plan:
 * a shape is matched only when every part of it is proven (docs/COL-PLAN.md).
 */
#ifndef SIFT_AGG_H
#define SIFT_AGG_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT bool sift_enable_agg;

/* Hooks and GUCs; from _PG_init */
extern void sift_agg_init(void);
extern void sift_aggcte_init(void);

/* sift_aggplan.c: called by the upper path hook of sift_planner.c */
extern void sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel,
								 RelOptInfo *output_rel, void *extra);

extern PGDLLIMPORT const CustomScanMethods sift_agg_plan_methods;

/* sift_aggcol.c: registers the sift_col index as a column source (does nothing in a build without the column engine) */
extern void sift_aggcol_register(void);

/* sift_agg.c: entry point of the parallel workers of a scan (access/parallel.h) */
struct dsm_segment;
struct shm_toc;
extern PGDLLEXPORT void sift_agg_worker_main(struct dsm_segment *seg, struct shm_toc *toc);

/* sift_agg.c: CreateCustomScanState callback */
extern Node *sift_agg_create_state(CustomScan *cscan);

#endif							/* SIFT_AGG_H */
