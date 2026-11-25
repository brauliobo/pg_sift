/*
 * agg/v6/exists_plan.h - WHERE EXISTS (SELECT .. FROM jsonb_to_recordset(fact.col) AS i(..) [LEFT] JOIN LATERAL (lookup .. LIMIT 1) m ON true WHERE <tests of the element and of the lookups>)
 * as a part of the join of the fact table (agg/v6/join/plan.c): the elements of the array are the unnest of the scan, each lookup a dimension probed by columns of the element and of the
 * relations of the statement (agg/v6/lateral_plan.h), and the tests run on the pairs. A row of the fact table counts once, when one of its pairs passes (SiftV6XElem.semi).
 *
 * PostgreSQL keeps the EXISTS as a correlated sub-plan filter of the join of the relations its arguments name (it has no equality to make a semi join of); the sub-select is read from the
 * planner it was planned under. Its relations take new numbers in the range table of the statement, after those it has.
 */
#ifndef SIFT_AGG_V6_EXISTS_PLAN_H
#define SIFT_AGG_V6_EXISTS_PLAN_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/join/shape.h"

/* the clause is an EXISTS sub-plan */
extern bool sift_v6_exists_clause(const RestrictInfo *ri);

/* The EXISTS clause ri of the join of sh as the elements, dimensions and conditions it adds to it; false when the sub-select is not of the shape above */
extern bool sift_v6_exists_join(PlannerInfo *root, SiftV6JoinShape *sh, RestrictInfo *ri);

/* The rows of the statement's own join come once for each row of the fact table, which the count of a row once needs: each dimension keeps one row for a key */
extern bool sift_v6_exists_main_unique(PlannerInfo *root, const SiftV6JoinShape *sh);

#endif							/* SIFT_AGG_V6_EXISTS_PLAN_H */
