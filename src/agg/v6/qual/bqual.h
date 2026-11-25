/*
 * agg/v6/qual/bqual.h - the quals of the scan of SiftAggV6 that no scan key says (SiftV6Spec.vqual): a column against a column (l_commitdate < l_receiptdate), an OR of quals, a numeric against a
 * constant of its own or another scale, any Boolean of the columns the machine of agg/vexpr/vexpr.h runs. The scan decodes the columns the clauses read for each batch and narrows its rows to those for
 * which every clause is true (an unknown is not), as PostgreSQL's executor does for a filter: a dense int4 or date column is handed to the machine as int32, a numeric as its scaled integer.
 * The clauses are over the columns of the relation as Vars of varno 1.
 */
#ifndef SIFT_AGG_V6_BQUAL_H
#define SIFT_AGG_V6_BQUAL_H

#include "postgres.h"

#include "agg/v6/vexpr.h"

#define SIFT_V6_BQ_RELID	1

typedef struct SiftV6Bq SiftV6Bq;
struct SiftV6Exec;

/* The clause is one the machine runs over the columns of the input (a relation as the planner or the executor opened it): true */
extern bool sift_v6_bq_plan_ok(PlannerInfo *root, const SiftV6Input *in, const Expr *clause);

/* The vector of a column of the input for the batches of the scan, or NULL when the scan does not decode it */
typedef const SiftVec *(*SiftV6BqCol) (void *arg, const SiftV6InCol *col);

/* exec: what the node prepared of the clauses that need the executor (v6_pg_exec.h); NULL when it has none */
extern SiftV6Bq *sift_v6_bq_open(MemoryContext cx, const List *quals, const SiftV6Input *in, SiftV6BqCol col, void *arg, const struct SiftV6Exec *exec);

/* The rows of bs the clauses leave: its mask, n_active and all_active are set */
extern void sift_v6_bq_filter(SiftV6Bq *bq, SiftBatchSel *bs);

#endif							/* SIFT_AGG_V6_BQUAL_H */
