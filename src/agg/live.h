/*
 * agg/live.h - the upper-path hook and the node registration of the operators of SiftAggV6 (topn, window, stream, sip, card) that the planner of the library (bm25/planner/planner.c) and a parallel worker
 * call; they live apart from both because the kernels of the aggregate side (agg/kern.h) and of the operators (agg/kern/kern.h) cannot be named in one translation unit. All of them do nothing while no format is bound.
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

#endif							/* SIFT_AGG_V6_LIVE_H */
