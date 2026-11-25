/*
 * agg/first/node.h - SiftAggV6First, DISTINCT ON of the rows of any plan without the sort (UPPERREL_DISTINCT): the node reads the cheapest plan of its input once and keeps the first row of each
 * group by the ORDER BY in a hash (v6_first.h), so a table whose index is sorted by nothing the statement names, a select list of arrays or expressions and the columns of a join take the same path.
 * It serves the statements over a table the bound format owns; the rows leave in no order, and PostgreSQL sorts the G groups when the statement orders them.
 */
#ifndef SIFT_AGG_V6_FIRST_NODE_H
#define SIFT_AGG_V6_FIRST_NODE_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_first_plan_methods;

extern void sift_agg_v6_first_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);

#endif							/* SIFT_AGG_V6_FIRST_NODE_H */
