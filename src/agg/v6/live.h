/*
 * agg/v6/live.h - the upper-path hook and the node registration of the operators of SiftAggV6 (topn, window, stream, sip, card) that the planner of the library (bm25/planner/planner.c) and a parallel worker
 * call; they live apart from both because the kernels of the aggregate side (agg/kern.h) and of the operators (agg/kern6/kern6.h) cannot be named in one translation unit. All of them do nothing while no format is bound.
 */
#ifndef SIFT_AGG_V6_LIVE_H
#define SIFT_AGG_V6_LIVE_H

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

/* create_upper_paths_hook: Top-N (UPPERREL_ORDERED), windows (UPPERREL_WINDOW, UPPERREL_DISTINCT) and streams of groups (UPPERREL_GROUP_AGG) */
extern void sift_agg_v6_live_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);

/* The names of the nodes, for a plan that a parallel worker reads */
extern void sift_agg_v6_live_register(void);

/* Whether the plan node is one of the operators of SiftAggV6 (the groups, the streams, the windows and the Top-N) and the index it reads; for the plans that price and inline CTEs (agg/cte/) */
extern bool sift_agg_v6_live_scan(const Plan *plan, Oid *indexoid);

/* Whether the index is one the bound format reads (an index of format major 2): the CTE rewrite treats a table with one as a table of a column index */
extern bool sift_agg_v6_live_owns_index(Oid indexoid);

#endif							/* SIFT_AGG_V6_LIVE_H */
