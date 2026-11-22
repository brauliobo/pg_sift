/*
 * agg/v6/v6_unnest_run.h - the aggregate scan of a statement that unnests an array column of the table in the FROM clause (CROSS JOIN LATERAL unnest(col) AS u(elem), or LEFT JOIN LATERAL ... ON true): the
 * rows of the index come a batch at a time with the lengths of their arrays and the element columns of the granule (col/f2/f2_sink_ext.h), the pairs of (row, element) are made on them
 * (agg/v6/v6_unnest.h) and go to the groups with the columns of the row beside the element, so that no row is built and no array is read. A row whose array the index cannot hold (a row
 * the write buffer still has, or a list longer than the index keeps) is read from the heap and its array taken apart as PostgreSQL does.
 */
#ifndef SIFT_AGG_V6_UNNEST_RUN_H
#define SIFT_AGG_V6_UNNEST_RUN_H

#include "postgres.h"

#include "agg/v6/v6.h"

extern void sift_v6_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats);

#endif							/* SIFT_AGG_V6_UNNEST_RUN_H */
