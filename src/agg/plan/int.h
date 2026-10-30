/*
 * agg/plan/int.h - what the files of agg/plan/ share
 * The typedefs two of them use and the functions one calls in another.
 */
#ifndef SIFT_AGG_PLAN_DIR_INT_H
#define SIFT_AGG_PLAN_DIR_INT_H

#include "agg/plan_int.h"

#define ARRAY_EQ_CONJ	8		/* the conjuncts an array equality comes to at most (array_eq_conjuncts) */

extern void spec_deps(PlannerInfo *root, SiftAggSpec *s);
extern Node * stats_expr(AggCtx *c, Node *e);

extern double set_groups(AggCtx *c, PlannerInfo *root, SiftAggSpec *s, List *keys, double rows, bool distinct);
extern void group_topn(PlannerInfo *root, SiftAggSpec *s, List *aggs, List *groupClause, GroupPathExtraData *gextra);

extern bool left_lateral(AggCtx *c, FromScan *fs, LeftJoin *lj, Index rti);

extern bool group_has_partition_key(AggCtx *c, List *keys);
extern bool agg_base_either(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs);

extern Plan *agg_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

#endif							/* SIFT_AGG_PLAN_DIR_INT_H */
