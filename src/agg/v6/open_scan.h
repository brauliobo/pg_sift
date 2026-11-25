/*
 * agg/v6/open_scan.h - the aggregate scan of a format that serves batches through SiftStoreOps.open alone (core/store/ops.h): it has no zone-pruned scan (zone is NULL), so the groups are
 * folded from the batches of its source, and the quals no scan key says (SiftV6Spec.vqual) narrow each batch. A statement is served this way when it reads plain columns of one relation.
 */
#ifndef SIFT_AGG_V6_OPEN_SCAN_H
#define SIFT_AGG_V6_OPEN_SCAN_H

#include "postgres.h"

#include "agg/v6/v6.h"

/* The statement reads plain columns of one relation, so the batches of open() answer it */
extern bool sift_v6_open_scan_ok(const SiftV6Spec *spec);

/* The groups of the statement from the batches of the source ops opens over the relation of the spec: heap is the table of an index, or the sift_table; index is NULL for a sift_table */
extern void sift_v6_open_scan_run(const SiftV6Spec *spec, SiftV6Group *group, const SiftStoreOps *ops, EState *estate, Relation heap, Relation index, SiftV6ScanStats *stats);

#endif							/* SIFT_AGG_V6_OPEN_SCAN_H */
