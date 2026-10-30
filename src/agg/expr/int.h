/*
 * agg/expr/int.h - what the files of agg/expr/ share
 * The typedefs two of them use and the functions one calls in another.
 */
#ifndef SIFT_AGG_EXPR_INT_H
#define SIFT_AGG_EXPR_INT_H

#include "agg/plan_int.h"

#define DAY_SECS		86400
#define MONTH_SECS		2629800
#define YEAR_SECS		31557600.0

extern const BucketUnit trunc_units[];
extern const BucketUnit field_units[];
extern bool session_zone_call(Oid funcid);
extern double bucket_groups(AggCtx *c, Node *e, double span);
extern bool utc_name(Node *e);
extern bool is_num_cast(Node *e);
extern bool is_utc_conversion(Node *e);

#endif							/* SIFT_AGG_EXPR_INT_H */
