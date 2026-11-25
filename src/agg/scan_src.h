/*
 * agg/scan_src.h - a relation the node of SiftAggV6 reads, as it opened it: a sift_col index with the keys of its scan, or a sift_table (no index) with the quals its format made of the
 * statement's. A partitioned table has one for each live partition.
 */
#ifndef SIFT_AGG_V6_SCAN_SRC_H
#define SIFT_AGG_V6_SCAN_SRC_H

#include "postgres.h"

#include "access/relscan.h"
#include "access/skey.h"
#include "utils/snapshot.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"

#include "core/store/ops.h"

typedef struct SiftV6ScanSrc
{
	Relation	heap;
	Relation	index;			/* a sift_col index; NULL for a sift_table, which heap is */
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *rtkeys;
	int			nrt;
	const SiftStoreOps *ops;	/* the format of the index, or of the sift_table */
	struct SiftStoreQuals *quals;	/* a sift_table: the keys and the predicates of its scan, made by its format */
	IndexFetchTableData *fetch;	/* a row Top-N: the winners of the source are fetched from its heap */
	TupleTableSlot *fetch_slot;
} SiftV6ScanSrc;

/*
 * Where an executor starts to read a source, opened by sift_v6_source_open: a plan made before the index went out of service is refused with SQLSTATE 40001 and planned again without it
 * (core/quarantine/quarantine.h), the scan counts in pg_stat_user_indexes (an index) or pg_stat_user_tables (a sift_table), and a SERIALIZABLE transaction takes the read lock of the whole table the scan reads, which a writer of another transaction is told about
 */
extern void sift_v6_source_enter(Relation source, bool is_table, Snapshot snap);

#endif							/* SIFT_AGG_V6_SCAN_SRC_H */
