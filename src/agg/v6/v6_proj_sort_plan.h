/*
 * agg/v6/v6_proj_sort_plan.h - SiftProjSortScan, the planner path and node that answer SELECT ... FROM t WHERE <predicates> ORDER BY <columns> LIMIT k from a SIFT_PROJ_SORT projection (the executor is
 * v6_proj_sort_exec.h) for a sift_col index and a sift_table alike. The path is offered for one plain base relation whose predicates are comparisons of its columns with constants, whose ORDER BY is
 * columns that a ready projection keeps in that order (or whose predicates its leading sort columns prune), and whose LIMIT is a constant; it carries the pathkeys, so no sort is planned above it.
 * Anything else (row level security, a security barrier, a volatile or non-constant expression, FOR UPDATE, WITH TIES, grouping, a join) never gets the path.
 *
 * The node fetches the relation's own row for every address the projection hands out, so the statement's quals are applied to the row the snapshot sees and its target list is computed from it. When
 * no projection serves the statement at execution (none stands for a part under the snapshot, a cell cannot be read) the node reads the relation and keeps the first k rows in a bounded sort.
 */
#ifndef SIFT_AGG_V6_PROJ_SORT_PLAN_H
#define SIFT_AGG_V6_PROJ_SORT_PLAN_H

#include "nodes/pathnodes.h"

extern void sift_v6_proj_sort_plan_init(void);

/* A ready projection serves the statement of the relation: its path, which carries the pathkeys, stands among those of the relation, and no generic top-N competes with it */
extern bool sift_v6_proj_sort_offered(const RelOptInfo *rel);

#endif							/* SIFT_AGG_V6_PROJ_SORT_PLAN_H */
