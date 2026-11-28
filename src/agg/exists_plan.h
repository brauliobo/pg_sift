/*
 * agg/exists_plan.h - WHERE EXISTS (SELECT .. FROM jsonb_to_recordset(fact.col) AS i(..) [LEFT] JOIN LATERAL (lookup .. LIMIT 1) m ON true WHERE <tests of the element and of the lookups>)
 * as a part of the join of the fact table (agg/join/plan.c): the elements of the array are the unnest of the scan, each lookup a dimension probed by columns of the element and of the
 * relations of the statement (agg/lateral_plan.h), and the tests run on the pairs. A row of the fact table counts once, when one of its pairs passes (SiftV6XElem.semi).
 *
 * PostgreSQL keeps the EXISTS as a correlated sub-plan filter of the join of the relations its arguments name (it has no equality to make a semi join of); the sub-select is read from the
 * planner it was planned under. Its relations take new numbers in the range table of the statement, after those it has.
 */
#ifndef SIFT_AGG_V6_EXISTS_PLAN_H
#define SIFT_AGG_V6_EXISTS_PLAN_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/join/shape.h"

/*
 * An EXISTS whose sub-select compares an element with a CASE of constants over the joined relations (`i.category = CASE s.kind WHEN 'a' THEN 'x' ELSE 'y' END`): a test of the row for each
 * constant, and the condition of the pairs the same CASE with the flag of each test as its result
 */
typedef struct SiftV6ExistsSplit
{
	const SubPlan *sp;
	const CaseExpr *inner;		/* the CASE as the sub-select has it, over the parameters of sp */
	const CaseExpr *cx;			/* the same in the terms of the statement */
} SiftV6ExistsSplit;

/* the clause is an EXISTS sub-plan */
extern bool sift_v6_exists_clause(const RestrictInfo *ri);

/* The EXISTS clause ri of the join of sh as the elements, dimensions and conditions it adds to it; false when the sub-select is not of the shape above */
extern bool sift_v6_exists_join(PlannerInfo *root, SiftV6JoinShape *sh, RestrictInfo *ri);

/* The EXISTS clause ri of the join of sh, which reads a joined relation by a CASE of constants only, as a split of sh (SiftV6ExistsSplit); false when it is not one */
extern bool sift_v6_exists_split(PlannerInfo *root, SiftV6JoinShape *sh, const RestrictInfo *ri);
/* The results of the CASE of a split, each once, and the one of no branch (its ELSE, or NULL) */
extern List *sift_v6_exists_split_values(const SiftV6ExistsSplit *sx);
extern const Const *sift_v6_exists_split_else(const SiftV6ExistsSplit *sx);

/* The rows of the statement's own join come once for each row of the fact table, which the count of a row once needs: each dimension keeps one row for a key. A RIGHT or FULL dimension
 * is probed by the row once it passed (agg/pipe.h): it may repeat a key */
extern bool sift_v6_exists_main_unique(PlannerInfo *root, const SiftV6JoinShape *sh);

#endif							/* SIFT_AGG_V6_EXISTS_PLAN_H */
