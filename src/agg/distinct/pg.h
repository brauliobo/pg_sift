/*
 * agg/distinct/pg.h - the key of a COUNT(DISTINCT) row from PostgreSQL datums: text, varchar, bpchar, bytea, uuid, numeric, the integer, date and time types, float4 and float8, in any number of
 * columns. Equal keys are the rows PostgreSQL's own comparison calls equal: a numeric is its normalized text (1.0 = 1.00), a bpchar has no trailing blanks, a float has one zero and one NaN, and a text
 * compares by its bytes, which is its equality in a deterministic collation (a nondeterministic one is refused: sift_distinct_collation_ok).
 */
#ifndef SIFT_AGG_V6_DISTINCT_PG_H
#define SIFT_AGG_V6_DISTINCT_PG_H

#include "postgres.h"

#include "agg/distinct/distinct.h"

extern bool sift_distinct_type_ok(Oid typid);
extern bool sift_distinct_collation_ok(Oid collid);
/* The columns of one row, in order: a NULL column is part of the key (the equality of two rows says a NULL equals a NULL) */
extern void sift_distinct_row_key(SiftDistinctKey *k, int ncols, const Oid *typids, const Datum *values, const bool *nulls);

#endif							/* SIFT_AGG_V6_DISTINCT_PG_H */
