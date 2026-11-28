/*
 * agg/pg_exec.h - the expressions of a spec that only the node that runs the scan can evaluate: a hashed
 * sub-plan (x NOT IN (SELECT ..)) and the output of an InitPlan (x = ANY (ARRAY (SELECT ..))). PostgreSQL starts them
 * under the PlanState of the node, so the node prepares them when it begins (BeginCustomScan): its sub-plans are its
 * own, EXPLAIN shows them and the executor ends them with the statement.
 */
#ifndef SIFT_AGG_V6_PG_EXEC_H
#define SIFT_AGG_V6_PG_EXEC_H

#include "postgres.h"

#include "nodes/execnodes.h"

#include "agg/engine.h"
#include "agg/expr/pg.h"

typedef struct SiftV6Exec
{
	PlanState  *ps;
	SiftV6PgExpr *qual;			/* the clauses of SiftV6Spec.vqual that need the executor, as one conjunction (NULL: none) */
	SiftV6PgExpr **item;		/* of each item of SiftV6Spec.pgexpr, its expression when it needs the executor, else NULL */
	ExprState  *heap_qual;		/* SiftV6Ext.heap_where, the test of a row an unnest reads again from the heap (NULL: none) */
} SiftV6Exec;

/* An expression context of the node, which heap_qual is evaluated in: its sub-plans and the outputs of its InitPlans are the node's (FreeExprContext when the scan ends) */
extern ExprContext *sift_v6_exec_context(const SiftV6Spec *spec);

#endif							/* SIFT_AGG_V6_PG_EXEC_H */
