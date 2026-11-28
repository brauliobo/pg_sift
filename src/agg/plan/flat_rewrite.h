/*
 * agg/plan/flat_rewrite.h - the rewrites of a statement before it is planned that let the aggregate scan take it (agg/plan/flat_rewrite.c)
 */
#ifndef SIFT_AGG_PLAN_FLAT_REWRITE_H
#define SIFT_AGG_PLAN_FLAT_REWRITE_H

#include "nodes/bitmapset.h"
#include "nodes/parsenodes.h"

/* in place, at every level of a SELECT: the LEFT JOIN LATERAL (.. LIMIT 1) that nothing reads is its left side */
extern void sift_agg_drop_unused_laterals(Query *q);

/*
 * The rewrites the caller plans against the statement as written; whether the statement has one, apply: they are made. The HAVING conditions of the rows moved to WHERE and the
 * WHERE conditions of the groups to HAVING, the plan
 * taken whenever it runs the aggregate scan (the rows form reads less in every shape measured, where the estimates may say otherwise) ...
 */
extern bool sift_agg_having_to_where(Query *q, bool apply);

/* ... a one-row sub-select without FROM joined LATERAL, its columns made the expressions it computes, or the outputs that may raise of one with FROM computed by the query, taken the same way ... */
extern bool sift_agg_inline_laterals(Query *q, bool apply);

/* ... and a scalar sub-select of one aggregate, an EXISTS or a LATERAL sub-select of aggregates tied to the query by equalities of columns made a join to the table grouped by them, taken when it costs less */
extern bool sift_agg_flat_decorrelate(Query *q, bool apply);

/*
 * lateral_inline.c, for agg/plan/lateral_union.c: may the columns raising (attnos) of the LATERAL entry rti move to where q reads them: the lateral is joined to the one table of a
 * grouped q for every row of it, no condition reads them and a key or an aggregate of every row does, so both plans evaluate them for the same rows
 */
extern bool sift_agg_lateral_raising_ok(Query *q, Index rti, Bitmapset *raising);

/* flat_rewrite.c, for agg/plan/lateral_inline.c: at most one row, and no effect but its rows: LIMIT 1 (not WITH TIES), no volatile function, no row lock, no data-modifying CTE */
extern bool sift_agg_one_row_query(Query *sq);
/* the expression with the columns of the grouping entry of 18 as the expressions they group by (in a sub-select of it too), as 17 parses it */
extern Node *sift_agg_ungroup_expr(Query *q, Node *n);

#endif							/* SIFT_AGG_PLAN_FLAT_REWRITE_H */
