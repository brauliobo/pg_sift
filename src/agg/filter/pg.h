/*
 * agg/filter/pg.h - the clause of an aggregate's FILTER (WHERE ..) from the quals the planner of SiftAggV6 makes of it: the forms of the quals of the scan (a column and a constant compared, IN of
 * constants, IS [NOT] NULL), each over an attribute of the index (INDEX_VAR), to the SiftClause that v6_filter_eval.h evaluates. The planner calls sift_v6_clause_make, which adds the columns the clause reads to
 * SiftV6Spec.fcol (the vectors the scan hands the group after those of the aggregates, sift_v6_spec_fvec) and gives the canonical bytes that tell one clause from another; the executor calls
 * sift_v6_clause_bind on the spec it unpacked. A constant the column does not compare with as an integer, a double or bytes (a date against a timestamp, whose comparison is of another unit) is refused.
 */
#ifndef SIFT_AGG_V6_FILTER_PG_H
#define SIFT_AGG_V6_FILTER_PG_H

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "agg/engine.h"
#include "agg/bytes.h"
#include "agg/filter/eval.h"

extern bool sift_v6_clause_make(MemoryContext cx, const List *quals, SiftV6Spec *spec, SiftClause *out, SiftBytes *canon);
/* the same for the clause of the elements of an unnest, whose conjuncts may be ORs of such tests (their rows are the union of the rows of the arms) */
extern bool sift_v6_clause_make_or(MemoryContext cx, const List *quals, SiftV6Spec *spec, SiftClause *out);
/* The aggregate with the FILTER of a, that the executor evaluates, as CASE WHEN filter THEN arg END of the same aggregate: NULL for an aggregate it does not fold (DISTINCT, ORDER BY, several arguments, a filter of native clauses) */
extern Aggref *sift_v6_filter_fold(Aggref *a);
extern bool sift_v6_clause_bind(MemoryContext cx, const List *quals, const SiftV6Spec *spec, SiftClause *out);

#endif							/* SIFT_AGG_V6_FILTER_PG_H */
