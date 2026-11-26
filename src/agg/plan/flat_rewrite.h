/*
 * agg/plan/flat_rewrite.h - the rewrites of a statement before it is planned that let the aggregate scan take it (agg/plan/flat_rewrite.c)
 */
#ifndef SIFT_AGG_PLAN_FLAT_REWRITE_H
#define SIFT_AGG_PLAN_FLAT_REWRITE_H

#include "nodes/parsenodes.h"

/* in place, at every level of a SELECT: the LEFT JOIN LATERAL (.. LIMIT 1) that nothing reads is its left side */
extern void sift_agg_drop_unused_laterals(Query *q);

/*
 * The rewrites the caller plans against the statement as written; whether the statement has one, apply: they are made. The HAVING conditions of the rows moved to WHERE, the plan
 * taken whenever it runs the aggregate scan (the rows form reads less in every shape measured, where the estimates may say otherwise) ...
 */
extern bool sift_agg_having_to_where(Query *q, bool apply);

/* ... and a scalar sub-select of one aggregate tied to the query by equalities of columns made a LEFT JOIN to the table grouped by them, taken when it costs less */
extern bool sift_agg_flat_decorrelate(Query *q, bool apply);

#endif							/* SIFT_AGG_PLAN_FLAT_REWRITE_H */
