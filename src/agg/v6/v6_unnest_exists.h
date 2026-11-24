/*
 * agg/v6/v6_unnest_exists.h - WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(col) AS i(c1 t1, ..) WHERE <quals of the element>) of the fact table as part of the aggregate scan of an unnest
 * (col/f2/f2_v6_unnest_run.c): the quals of the element are decided on the leaves of the elements, all on the same element, and a row of the table is one pair when one of its elements passes them,
 * with no document read. PostgreSQL keeps the EXISTS as a correlated sub-plan filter of the table (it has no equality to make a semi join of); its sub-select is read from the root the sub-plan was
 * planned under.
 *
 * Other quals of the table are those of any unnest (agg/v6/v6_unnest_plan.c); keys and aggregates name columns of the table only (the element has no name outside the EXISTS).
 */
#ifndef SIFT_AGG_V6_UNNEST_EXISTS_H
#define SIFT_AGG_V6_UNNEST_EXISTS_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/plan_int.h"

/* The path of the scan of the statement over one table when one of the quals of the table is such an EXISTS; nothing when the statement is not of that shape */
extern void sift_v6_unnest_exists_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs);

#endif							/* SIFT_AGG_V6_UNNEST_EXISTS_H */
