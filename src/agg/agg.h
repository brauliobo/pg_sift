/*
 * agg/agg.h - SiftAgg: aggregate queries run on the column source (agg/colsrc.h) instead of the heap.
 *
 * agg/plan.c is the planner side (shape matcher in create_upper_paths_hook, cost), agg/agg.c the executor
 * (Custom Scan (SiftAgg)), agg/kern*.c the vectorized operators. The query's result is the one of the plain plan:
 * a shape is matched only when every part of it is proven (docs/COL-PLAN.md).
 */
#ifndef SIFT_AGG_H
#define SIFT_AGG_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT bool sift_enable_agg;
extern PGDLLIMPORT double sift_plan_scale;
extern PGDLLIMPORT double sift_agg_scale_horizon;
extern PGDLLIMPORT bool sift_agg_two_level_on;
extern PGDLLIMPORT bool sift_agg_preagg;
extern PGDLLIMPORT bool sift_agg_super;
extern PGDLLIMPORT bool sift_agg_dist_stream;
extern PGDLLIMPORT bool sift_agg_join_filter;
extern PGDLLIMPORT bool sift_agg_left_lateral;
extern PGDLLIMPORT bool sift_enable_topn;
extern PGDLLIMPORT bool sift_topn_walk;
extern PGDLLIMPORT int sift_agg_param_set_size;
extern PGDLLIMPORT bool sift_agg_key_skew;
extern PGDLLIMPORT int sift_agg_lookup_max_rows;
extern PGDLLIMPORT int sift_agg_lookup_max_kb;
extern bool agg_lookup_fits(double rows, int width);

/* agg/first.c: does a unique index of the table hold only columns among atts */
extern bool sift_agg_unique_key(Oid relid, int nkey, int nord, const int *atts);

/* Hooks and GUCs; from _PG_init */
extern void sift_agg_init(void);
extern void sift_aggcte_init(void);
extern MemoryContext sift_agg_memo_begin(void);
extern void sift_agg_memo_end(MemoryContext cx);
extern bool sift_agg_has_flattenable_laterals(Query *q);
extern void sift_agg_flatten_laterals(Query *q);

/* agg/plan.c: called by the upper path hook of bm25/planner.c */
extern void sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel,
								 RelOptInfo *output_rel, void *extra);

extern PGDLLIMPORT const CustomScanMethods sift_agg_plan_methods;

/* agg/col.c: registers the sift_col index as a column source (does nothing in a build without the column engine) */
extern void sift_aggcol_register(void);

/* agg/par.c: entry point of the parallel workers of a scan (access/parallel.h) */
struct dsm_segment;
struct shm_toc;
extern PGDLLEXPORT void sift_agg_worker_main(struct dsm_segment *seg, struct shm_toc *toc);

/* agg/agg.c: CreateCustomScanState callback */
extern Node *sift_agg_create_state(CustomScan *cscan);

#endif							/* SIFT_AGG_H */
