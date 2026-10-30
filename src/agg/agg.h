/*
 * agg/agg.h - SiftAgg: aggregate queries run on the column source (agg/colsrc.h) instead of the heap.
 *
 * agg/plan/ is the planner side (shape matcher in create_upper_paths_hook, cost), agg/agg.c the executor
 * (Custom Scan (SiftAgg)), agg/kern*.c the vectorized operators. The query's result is the one of the plain plan:
 * a shape is matched only when every part of it is proven (docs/COL-PLAN.md).
 */
#ifndef SIFT_AGG_H
#define SIFT_AGG_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT bool sift_enable_agg;
extern PGDLLIMPORT bool sift_agg_force;

/*
 * The passes that rewrite a query before it is planned (agg/flat/, agg/preinline.c, agg/cte/rewrite.c, core/jsonbuild.c) look for sub-selects, CTE
 * references and calls; a list of constants (`k IN (1, 2, ..)`) holds none, and walking it costs about 570 instructions a key over all of them.
 * SIFT_SKIP_CONST_LIST(n, ret) is the first line of their walkers, after the NULL test.
 */
static inline bool
sift_const_list(Node *n)
{
	if (!IsA(n, ArrayExpr))
		return false;
	foreach_ptr(Node, e, ((ArrayExpr *) n)->elements)
		if (!IsA(e, Const))
			return false;
	return true;
}

#ifdef SIFT_TESTING
extern int64 sift_prepass_nodes;	/* the nodes the passes visited; sift._test_prepass_nodes */
#define SIFT_SKIP_CONST_LIST(n, ret) \
	do { sift_prepass_nodes++; if (sift_const_list(n)) return ret; } while (0)
#else
#define SIFT_SKIP_CONST_LIST(n, ret) \
	do { if (sift_const_list(n)) return ret; } while (0)
#endif

/*
 * agg/cost/join.c: does probing the keys of a dependent lookup in a btree of the table cost less than reading its pages (the loader's rule and the price's), and the rows a key has
 * (the table's rows over the distinct values of the column by its statistics; 0: unknown)
 */
extern void sift_agg_walks_forget(void);
extern int64 sift_agg_outer_asks;
extern int64 sift_agg_outer_walks;	/* SIFT_TESTING: the walks for references to a query above that the plan made */
extern bool sift_agg_probe_pays(double keys, double rows, double pages);
extern double sift_agg_rows_per_key(unsigned int relid, int att, double tuples);
extern PGDLLIMPORT double sift_plan_scale;
extern PGDLLIMPORT double sift_agg_scale_horizon;
extern PGDLLIMPORT bool sift_agg_two_level_on;
extern PGDLLIMPORT bool sift_agg_preagg;
extern PGDLLIMPORT int sift_agg_dead_delta;
extern PGDLLIMPORT bool sift_agg_super;
extern PGDLLIMPORT bool sift_agg_dist_stream;
extern PGDLLIMPORT bool sift_agg_join_filter;
extern PGDLLIMPORT bool sift_agg_left_lateral;
extern PGDLLIMPORT bool sift_enable_topn;
extern PGDLLIMPORT bool sift_topn_walk;
extern PGDLLIMPORT int sift_agg_param_set_size;
extern PGDLLIMPORT int sift_agg_par_min_rows;
extern PGDLLIMPORT bool sift_agg_key_skew;
extern PGDLLIMPORT int sift_agg_lookup_max_rows;
extern PGDLLIMPORT int sift_agg_lookup_max_kb;
extern bool agg_lookup_fits(double rows, int width);

/* agg/first.c: does a unique index of the table hold only columns among atts */
extern bool sift_agg_unique_key(Oid relid, int nkey, int nord, const int *atts, List **cols);

/* Hooks and GUCs; from _PG_init */
extern void sift_agg_init(void);
extern void sift_aggcte_init(void);
extern MemoryContext sift_agg_memo_begin(void);
extern void sift_agg_memo_end(MemoryContext cx);
extern void sift_agg_preinline(Query *q);
#ifdef SIFT_TESTING
extern int64 sift_source_lookups;	/* the asks of has_column_source that went to the catalog */
#endif
extern bool sift_agg_has_flattenable_laterals(Query *q);
extern bool sift_agg_flatten_laterals(Query *q);
extern bool sift_agg_by_cost;			/* the flattening made a rewrite whose plan is chosen by cost, not taken for running in the scan */
extern bool sift_agg_latest_per_group(Query *q, bool apply);

/* agg/plan/paths.c: called by the upper path hook of bm25/planner.c */
extern void sift_agg_minmax_paths(PlannerInfo *root, RelOptInfo *rel);
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
