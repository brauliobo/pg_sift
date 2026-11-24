/*
 * agg/v6/v6_unnest_run.h - the aggregate scan of a statement that unnests an array column of the table in the FROM clause (CROSS JOIN LATERAL unnest(col) AS u(elem), or LEFT JOIN LATERAL ... ON true): the
 * rows of the index come a batch at a time with the lengths of their arrays and the element columns of the granule (col/f2/f2_sink_ext.h), the pairs of (row, element) are made on them
 * (agg/v6/v6_unnest.h) and go to the groups with the columns of the row beside the element, so that no row is built and no array is read. A row whose array the index cannot hold (a row
 * the write buffer still has, or a list longer than the index keeps) is read from the heap and its array taken apart as PostgreSQL does.
 */
#ifndef SIFT_COL_F2_V6_UNNEST_RUN_H
#define SIFT_COL_F2_V6_UNNEST_RUN_H

#include "postgres.h"

#include "agg/v6/v6.h"

struct SiftV6StarRun;

/* One pass over the dimensions of star (NULL: the unnest reads no other relation) */
extern void sift_v6_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, const struct SiftV6StarRun *star);

/*
 * The same for a sift_table, whose store keeps no element streams: the scan of the table (f2_v6_scan.c) gives the columns of the rows a batch at a time and each row's array is taken apart as
 * PostgreSQL does, from its heap row. nfact vectors of fact_type are the columns of the scan; attr_slot says which of them a column of the statement is.
 */
typedef struct UnRun SiftV6UnnestRows;

extern SiftV6UnnestRows *sift_v6_unnest_rows_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Snapshot snap, SiftV6ScanStats *stats, int nfact, const SiftVecType *fact_type,
												   int (*attr_slot) (void *arg, const SiftV6Col *c), void *attr_arg);
extern void sift_v6_unnest_rows_run(SiftV6UnnestRows *u, SiftVec *const *fact, const SiftBatchSel *bs, const uint64 *tid);
extern void sift_v6_unnest_rows_end(SiftV6UnnestRows *u);

#endif							/* SIFT_COL_F2_V6_UNNEST_RUN_H */
