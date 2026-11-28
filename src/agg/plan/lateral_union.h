/*
 * agg/plan/lateral_union.h - a LATERAL UNION [ALL] of sub-selects without FROM, `JOIN LATERAL (SELECT e1 UNION ALL SELECT e2 ..) c ON true`, gives for each row of its left side one row
 * of each arm (UNION: of each arm whose row no earlier arm gave). It is the sub-select of the arms' numbers, `SELECT CASE a.i WHEN 1 THEN e1 WHEN 2 THEN e2 .. END FROM (VALUES (1), (2) ..) a(i)`,
 * which a UNION keeps only for an arm whose row is distinct from the rows of the arms before it, as the set operation compares them. PostgreSQL pulls that sub-select up: the arms are a small
 * relation the aggregate scan joins to every row, and the outputs expressions of the joined rows.
 */
#ifndef SIFT_AGG_PLAN_LATERAL_UNION_H
#define SIFT_AGG_PLAN_LATERAL_UNION_H

#include "nodes/parsenodes.h"

/* whether a sub-select of the FROM lists of q (at any level) is such a set operation, read by the relations before it; apply: each is made the sub-select of its arms' numbers */
extern bool sift_agg_lateral_union(Query *q, bool apply);

#endif							/* SIFT_AGG_PLAN_LATERAL_UNION_H */
