/*
 * agg/join/join.h - the joins of SiftAggV6 that are not an aggregate of one table: the partitioned (Grace) hash join of join.c and the ASOF join of join_asof.c. Each is a CustomScan the planner offers
 * for a join of the statement through the join path hook (the one of bm25/planner/planner.c hands every join to sift_agg_v6_join_paths and sift_agg_v6_asof_paths), only when a relation under the join has an
 * index of format major 2; PostgreSQL's own join stays among the paths, and the cheapest is the one planned. The node computes everything when it is first asked, into a tuplestore that spills past
 * work_mem (SiftV6Store of v6_vexpr.h), so that the parent reads the rows of the join as it reads any scan.
 */
#ifndef SIFT_AGG_V6_JOIN_H
#define SIFT_AGG_V6_JOIN_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_join_plan_methods;
extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_asof_plan_methods;

extern void sift_agg_v6_join_paths(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra);
extern void sift_agg_v6_asof_paths(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra);

/* Has a base relation among relids an index of the format of SiftAggV6? The joins are offered for those statements only */
extern bool sift_v6_rels_have_index(PlannerInfo *root, Relids relids);

/* The resno of the entry of a plan's target list that is the Var v (varno and varattno), 0 when none is: the column of the child that gives it */
extern int	sift_v6_child_resno(const List *tlist, const Var *v);
/* whether a Var is one of the columns a path outputs (its pathtarget holds expressions, not target entries) */
extern bool sift_v6_target_has(const PathTarget *target, const Var *v);

/* The paths of a base relation under an index of format major 2 (v6_cost.c): its rows from the statistics of the index when they are trusted, then the scan with the filter of its join */
extern void sift_agg_v6_rel_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);

#endif							/* SIFT_AGG_V6_JOIN_H */
