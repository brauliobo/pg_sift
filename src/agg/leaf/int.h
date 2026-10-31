/*
 * agg/leaf/int.h - what the files of agg/leaf/ share
 * The leaves of the column source that an expression of the query reads, and the functions the files call each other by (the ones other directories
 * call are in agg/plan_int.h).
 */
#ifndef SIFT_AGG_LEAF_INT_H
#define SIFT_AGG_LEAF_INT_H

#include "agg/plan_int.h"

extern int scalar_leaf(AggCtx *c, int att, List *path, Oid typid);
extern int array_lane_leaf(AggCtx *c, Node *e, Oid fid);
extern int key_leaf(AggCtx *c, int att, List *path, Oid typid);
extern int types_leaf(AggCtx *c, Node *call);
extern int count_leaf(AggCtx *c, int grp);
extern int member_leaf(AggCtx *c, Oid typid);
extern int func_var_leaf(AggCtx *c, Var *v);
extern int array_length_leaf(AggCtx *c, Node *e);
extern int each_value_cast_leaf(AggCtx *c, Node *e, Oid fid, Oid cast_to, const JsonRef *r);
extern int json_path_leaf(AggCtx *c, Node *e, Oid fid, Oid cast_to, const JsonRef *r);

#endif
