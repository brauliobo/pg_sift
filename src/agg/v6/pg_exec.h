/*
 * agg/v6/pg_exec.h - the expressions of a spec that only the node that runs the scan can evaluate: a hashed
 * sub-plan (x NOT IN (SELECT ..)) and the output of an InitPlan (x = ANY (ARRAY (SELECT ..))). PostgreSQL starts them
 * under the PlanState of the node, so the node prepares them when it begins (BeginCustomScan): its sub-plans are its
 * own, EXPLAIN shows them and the executor ends them with the statement.
 */
#ifndef SIFT_AGG_V6_PG_EXEC_H
#define SIFT_AGG_V6_PG_EXEC_H

#include "postgres.h"

#include "nodes/execnodes.h"

#include "agg/v6/v6.h"
#include "agg/v6/expr/pg.h"

typedef struct SiftV6Exec
{
	PlanState  *ps;
	SiftV6PgExpr *qual;			/* the clauses of SiftV6Spec.vqual that need the executor, as one conjunction (NULL: none) */
	SiftV6PgExpr **item;		/* of each item of SiftV6Spec.pgexpr, its expression when it needs the executor, else NULL */
} SiftV6Exec;

#endif							/* SIFT_AGG_V6_PG_EXEC_H */
