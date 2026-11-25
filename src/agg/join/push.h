/*
 * agg/join/push.h - an aggregate of a fact table joined to a small relation on an integer column, answered by the scan of the fact table's sift_col index with the join column restricted to
 * the values the small side holds: the small side is run first and its column becomes a sorted array that the scan takes as an `= ANY` key, so that the zone maps and the vectorized filters of the
 * scan do the join. The join must neither multiply nor drop fact rows: the small side is unique on the join column once its equalities to constants are given, or the join is the semi-join of an IN
 * (sub-select). A statement that has a scope summary for the pairs is answered by col/f2/agg/scope/scope.c when the summary is current; this path is the one for the rest.
 */
#ifndef SIFT_AGG_V6_JOIN_PUSH_H
#define SIFT_AGG_V6_JOIN_PUSH_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/engine.h"

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_push_plan_methods;

extern void sift_agg_v6_join_push_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);


#endif							/* SIFT_AGG_V6_JOIN_PUSH_H */
