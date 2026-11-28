/*
 * agg/plan/lineage.h - the conjuncts of an IN sub-select of an inlined shared CTE that the CTE it reads already keeps its rows by (agg/plan/cte.c).
 */
#ifndef SIFT_AGG_PLAN_LINEAGE_H
#define SIFT_AGG_PLAN_LINEAGE_H

#include "postgres.h"

#include "nodes/primnodes.h"

/*
 * A shared CTE written inline may restate, in an IN sub-select over a CTE, the condition that CTE already keeps its rows by. Each conjunct of the sub-select that the
 * rows of that CTE satisfy by their lineage is dropped, and the sub-select then reads the CTE alone; true when one was. ctes: the CTEs of the WITH before the body.
 */
extern bool sift_agg_cte_drop_restated(SubLink *sl, List *ctes);

#endif							/* SIFT_AGG_PLAN_LINEAGE_H */
