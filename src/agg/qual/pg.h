/*
 * agg/qual/pg.h - SiftV6PgQual: the quals of the scan (SiftV6Spec.vqual) that no scan key says and the machine of agg/vexpr/vexpr.h does not run: LIKE, ILIKE and ~ of any pattern, IS DISTINCT FROM, = ANY and
 * <> ALL, CASE, a SQL or PL function, an arbitrary operator, a stable function (date_trunc of a timestamptz, to_char) - any Boolean expression over the columns the scan decodes, evaluated by
 * PostgreSQL's executor (v6_pg_expr.h) for the rows of a batch that the zone maps, the scan keys and the native predicates have left. An unknown is not a pass, as for a filter.
 * The clauses are over the columns of the relation as Vars of varno SIFT_V6_BQ_RELID (v6_bqual.h).
 */
#ifndef SIFT_AGG_V6_PG_QUAL_H
#define SIFT_AGG_V6_PG_QUAL_H

#include "postgres.h"

#include "agg/qual/bqual.h"
#include "agg/expr/pg.h"

typedef struct SiftV6PgQual SiftV6PgQual;

/* The clause is a Boolean PostgreSQL expression whose columns the input decodes (each one a column of a class the scan reads) */
extern bool sift_v6_pgqual_plan_ok(PlannerInfo *root, const SiftV6Input *in, const Expr *clause);

/* The conjunction of the clauses of quals that need the executor (sift_v6_pgexpr_exec), made under ps in cx, which lives as long as the node; NULL when no clause does */
extern SiftV6PgExpr *sift_v6_pgqual_prepare(MemoryContext cx, PlanState *ps, const List *quals);

/* The conjunction of the quals, evaluated by prepared when it is not NULL (the one sift_v6_pgqual_prepare made of them); the columns they read are added to in, and src[slot] (filled by the caller for the columns of in once this returns) is what filter reads */
extern SiftV6PgQual *sift_v6_pgqual_open(MemoryContext cx, const List *quals, SiftV6Input *in, SiftV6PgExpr *prepared);

/* The rows of bs that the quals pass (an active row only): its mask, n_active and all_active are set; src[slot] is the vector of input column slot */
extern void sift_v6_pgqual_filter(SiftV6PgQual *q, SiftBatchSel *bs, const SiftVec *const *src);

#endif							/* SIFT_AGG_V6_PG_QUAL_H */
