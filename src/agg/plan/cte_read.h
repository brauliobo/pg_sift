/*
 * agg/plan/cte_read.h - whether planning a shared CTE inline keeps the errors of its shared copy. The copy computes every column of a row a reader takes, the inline plan only the
 * columns each reader evaluates: an expression of the body that may raise (a cast, a division) raises for the same rows both ways when every reader is an aggregate over the CTE alone
 * that takes all its rows and evaluates the column on each of them, in a part of its statement the planner keeps.
 */
#ifndef SIFT_AGG_PLAN_CTE_READ_H
#define SIFT_AGG_PLAN_CTE_READ_H

#include "postgres.h"

#include "nodes/parsenodes.h"

/* top: the statement; owner: the query level whose WITH holds cte. True when the body raises nowhere but in its output columns and each reader evaluates those columns on every row */
extern bool sift_cte_readers_evaluate_raising(Query *top, const Query *owner, const CommonTableExpr *cte);

#endif							/* SIFT_AGG_PLAN_CTE_READ_H */
