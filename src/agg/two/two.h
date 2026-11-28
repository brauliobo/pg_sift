/*
 * agg/two/two.h - SiftAggV6Two, the two-level aggregate: an outer aggregate over a sub-select (or a UNION ALL of them) whose arms are SiftAggV6 aggregates of one scan, each
 * grouped by a unique row id. The arms run as they plan (they share one pass of the index, agg/fuse/fuse.h) and every group of an arm (a cell: the share of one row in one
 * group) folds into the groups of the outer aggregate in the node, so no grouping of PostgreSQL runs above the arms.
 */
#ifndef SIFT_AGG_TWO_H
#define SIFT_AGG_TWO_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

/* the outer statement over the arms at UPPERREL_GROUP_AGG: the path of the node, or the refusal of the arm "two_level" */
extern void sift_agg_v6_two_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_two_plan_methods;

#endif							/* SIFT_AGG_TWO_H */
