/*
 * agg/v6/v6_unnest_exists.h - tests of the elements of an array column of the fact table as part of the aggregate scan of an unnest (col/f2/f2_v6_unnest_run.c): WHERE [NOT] EXISTS (SELECT 1 FROM
 * jsonb_to_recordset(col) AS i(c1 t1, ..) WHERE <quals of the element>), the same inside an aggregate's FILTER, jsonb_path_exists and containment (agg/v6/v6_unnest_path.h). The quals of the element are
 * decided on the leaves of the elements, all on the same element, with the comparisons of an element with a column of its row; the result is a boolean of the row, which the WHERE keeps or a FILTER
 * reads as the virtual column SIFT_V6_XFLAG_VATT + the test, with no document read. PostgreSQL keeps an EXISTS as a correlated sub-plan (it has no equality to make a semi join of); its sub-select is
 * read from the root the sub-plan was planned under.
 *
 * Other quals of the table are those of any unnest (agg/v6/v6_unnest_plan.c); keys and aggregates name columns of the table only (the element has no name outside the test).
 */
#ifndef SIFT_AGG_V6_UNNEST_EXISTS_H
#define SIFT_AGG_V6_UNNEST_EXISTS_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/plan_int.h"

/* The path of the scan of the statement over one table when a qual of the table or a FILTER is such a test; nothing when the statement is not of that shape */
extern void sift_v6_unnest_exists_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs);

#endif							/* SIFT_AGG_V6_UNNEST_EXISTS_H */
