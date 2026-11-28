/*
 * agg/v6_unnest_run.h - the aggregate scan of a statement that unnests an array column of the table in the FROM clause (CROSS JOIN LATERAL unnest(col) AS u(elem), or LEFT JOIN LATERAL ... ON true): the
 * rows of the index come a batch at a time with the lengths of their arrays and the element columns of the granule (col/f2/sink/ext.h), the pairs of (row, element) are made on them
 * (agg/unnest/unnest.h) and go to the groups with the columns of the row beside the element, so that no row is built and no array is read. A sift_table gives its rows and elements the
 * same way, from its store. A row whose array the leaves cannot hold (a row the write buffer still has, a list longer than the relation keeps, an array no path declares) is read again by its
 * address and its array taken apart as PostgreSQL does.
 */
#ifndef SIFT_COL_F2_V6_UNNEST_RUN_H
#define SIFT_COL_F2_V6_UNNEST_RUN_H

#include "postgres.h"

#include "agg/engine.h"
#include "core/store/ops.h"

struct SiftV6StarRun;

/* One pass over the dimensions of star (NULL: the unnest reads no other relation); index NULL: heap is a sift_table, whose store gives the rows and the elements */
extern void sift_v6_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, SiftV6ScanStats *stats,
							   const struct SiftV6StarRun *star);

/* The arms of a cohort over one pull of the index (arms.c); alone[i]: the leaves of arm i did not fit the shared pull and it read its own after it */
struct SiftV6FuseCohort;
extern void sift_v6_unnest_run_arms(struct SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, Relation heap, Relation index, Snapshot snap, bool *alone);
/* The same in the participants of a parallel run, by the units each claims (col/f2/agg/unnest/morsel.c) */
extern const SiftStoreParOps sift_f2_unnest_par_ops;

#endif							/* SIFT_COL_F2_V6_UNNEST_RUN_H */
