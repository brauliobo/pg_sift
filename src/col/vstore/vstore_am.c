/*
 * col/vstore/vstore_am.c - the table access method sift_values of the relation of a shared value store (col/vstore/vstore.h)
 * A store has no rows: a scan returns none, nothing inserts, indexes and TRUNCATE are refused; its pages are written by sift (core/store.h).
 * Everything that a relation can still be asked (size, copy of the file, ANALYZE) is answered for an empty table, except that a rewrite
 * (VACUUM FULL, CLUSTER, ALTER TABLE ... SET TABLESPACE) copies the pages as they are, so that the block numbers of the directory stay valid.
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/tableam.h"
#include "catalog/storage.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/vstore/vstore.h"
#include "table/table.h"

PG_FUNCTION_INFO_V1(sift_values_handler);

extern const TableAmRoutine *sift_tbl_routine(void);

static void
vs_refuse(Relation rel)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("\"%s\" is a shared value store, managed by sift", RelationGetRelationName(rel)),
			 errhint("Create or drop the sift_col indexes of its table instead.")));
}

static TableScanDesc
vs_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
	TableScanDesc scan = palloc0(sizeof(TableScanDescData));

	scan->rs_rd = rel;
	scan->rs_snapshot = snapshot;
	scan->rs_flags = flags;
	scan->rs_parallel = pscan;
	return scan;
}

static void
vs_scan_end(TableScanDesc scan)
{
	if (scan->rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_snapshot);
	pfree(scan);
}

static void
vs_scan_rescan(TableScanDesc scan, struct ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode)
{
}

static bool
vs_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	ExecClearTuple(slot);
	return false;
}

static void
vs_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid)
{
}

static bool
vs_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return false;
}

static bool
vs_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows, double *deadrows, TupleTableSlot *slot)
{
	return false;
}

static bool
vs_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate)
{
	return false;
}

static bool
vs_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate, TupleTableSlot *slot)
{
	return false;
}

#if PG_VERSION_NUM >= 180000
static bool
vs_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages, uint64 *exact_pages)
{
	return false;
}
#else
static bool
vs_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres)
{
	return false;
}

static bool
vs_scan_bitmap_next_tuple(TableScanDesc scan, TBMIterateResult *tbmres, TupleTableSlot *slot)
{
	return false;
}
#endif

static void
vs_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, struct BulkInsertStateData *bistate)
{
	vs_refuse(rel);
}

static void
vs_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, struct BulkInsertStateData *bistate,
							uint32 specToken)
{
	vs_refuse(rel);
}

static void
vs_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
	vs_refuse(rel);
}

static void
vs_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, SiftTamOptions options, struct BulkInsertStateData *bistate)
{
	vs_refuse(rel);
}

static void
vs_nontransactional_truncate(Relation rel)
{
	vs_refuse(rel);
}

static double
vs_index_build_range_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, bool allow_sync, bool anyvisible, bool progress,
						  BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	vs_refuse(table_rel);
	return 0;
}

static void
vs_index_validate_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, Snapshot snapshot, struct ValidateIndexState *state)
{
	vs_refuse(table_rel);
}

/* A relation that already has storage under another file number is being truncated (a rewrite makes a new relation, a copy keeps the pages) */
static void
vs_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	if (!RelFileLocatorEquals(rel->rd_locator, *newrlocator))
		vs_refuse(rel);
	sift_tbl_routine()->relation_set_new_filelocator(rel, newrlocator, persistence, freezeXid, minmulti);
}

static void
vs_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy)
{
}

/* The pages as they are: the directory names blocks */
static void
vs_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, TransactionId OldestXmin,
					SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed,
					double *tups_recently_dead)
{
	*num_tuples = *tups_vacuumed = *tups_recently_dead = 0;
	*xid_cutoff = OldestXmin;
	*multi_cutoff = GetOldestMultiXactId();
	FlushRelationBuffers(OldTable);
	RelationCopyStorage(RelationGetSmgr(OldTable), RelationGetSmgr(NewTable), MAIN_FORKNUM, NewTable->rd_rel->relpersistence);
}

static void
vs_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
	*pages = RelationGetNumberOfBlocks(rel);
	*tuples = 0;
	*allvisfrac = 0;
}

static TableAmRoutine vs_methods;

Datum
sift_values_handler(PG_FUNCTION_ARGS)
{
	if (vs_methods.type != T_TableAmRoutine)
	{
		vs_methods = *sift_tbl_routine();
		vs_methods.scan_begin = vs_scan_begin;
		vs_methods.scan_end = vs_scan_end;
		vs_methods.scan_rescan = vs_scan_rescan;
		vs_methods.scan_getnextslot = vs_scan_getnextslot;
		vs_methods.scan_set_tidrange = vs_scan_set_tidrange;
		vs_methods.scan_getnextslot_tidrange = vs_scan_getnextslot;
		vs_methods.parallelscan_estimate = table_block_parallelscan_estimate;
		vs_methods.parallelscan_initialize = table_block_parallelscan_initialize;
		vs_methods.parallelscan_reinitialize = table_block_parallelscan_reinitialize;
		vs_methods.scan_analyze_next_block = vs_scan_analyze_next_block;
		vs_methods.scan_analyze_next_tuple = vs_scan_analyze_next_tuple;
		vs_methods.scan_sample_next_block = vs_scan_sample_next_block;
		vs_methods.scan_sample_next_tuple = vs_scan_sample_next_tuple;
#if PG_VERSION_NUM < 180000
		vs_methods.scan_bitmap_next_block = vs_scan_bitmap_next_block;
#endif
		vs_methods.scan_bitmap_next_tuple = vs_scan_bitmap_next_tuple;
		vs_methods.tuple_insert = vs_tuple_insert;
		vs_methods.tuple_insert_speculative = vs_tuple_insert_speculative;
		vs_methods.tuple_complete_speculative = vs_tuple_complete_speculative;
		vs_methods.multi_insert = vs_multi_insert;
		vs_methods.relation_nontransactional_truncate = vs_nontransactional_truncate;
		vs_methods.index_build_range_scan = vs_index_build_range_scan;
		vs_methods.index_validate_scan = vs_index_validate_scan;
		vs_methods.relation_set_new_filelocator = vs_set_new_filelocator;
		vs_methods.relation_vacuum = vs_relation_vacuum;
		vs_methods.relation_copy_for_cluster = vs_copy_for_cluster;
		vs_methods.relation_estimate_size = vs_relation_estimate_size;
	}
	PG_RETURN_POINTER(&vs_methods);
}
