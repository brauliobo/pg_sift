/*
 * agg/plan_join_int.h - shared between agg/plan_join.c and agg/plan_child.c
 */
#ifndef SIFT_AGG_PLAN_JOIN_INT_H
#define SIFT_AGG_PLAN_JOIN_INT_H

#include "agg/plan_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* plan_child.c */
bool projects_columns(Path *p);
Path *child_path(RelOptInfo *rel, bool correlated);
bool member_arg(AggCtx *c, Node *arg);
Node *plain_child_sig(AggCtx *c, RangeTblEntry *e, Index rti, RelOptInfo *rel);
bool planned_correlated_child(AggCtx *c, RangeTblEntry *e, RelOptInfo *rel, JoinCtx *jc);
bool ancestor_param_walker(Node *n, PlannerInfo *root);

#pragma GCC visibility pop

#endif
