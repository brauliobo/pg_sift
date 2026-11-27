/*
 * agg/plan/lineage.h - the conjuncts of an IN sub-select of an inlined shared CTE that the CTE it reads already keeps its rows by (agg/plan/cte.c).
 */
#ifndef SIFT_AGG_PLAN_LINEAGE_H
#define SIFT_AGG_PLAN_LINEAGE_H

#include "postgres.h"

#include "nodes/pg_list.h"

/*
 * A shared CTE written inline may restate, in an IN sub-select over a CTE, the condition that CTE already keeps its rows by. Each conjunct of quals (the WHERE of the body) that
 * the rows of that CTE satisfy by their lineage is dropped from the sub-select, which then reads the CTE alone. ctes: the CTEs of the WITH before the body.
 */
extern void sift_agg_cte_drop_restated(Node *quals, List *ctes);

#endif							/* SIFT_AGG_PLAN_LINEAGE_H */
