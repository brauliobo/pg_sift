/*
 * table/am.c - the table access method sift_table: the handler and the routing of its callbacks to the table of the format of the relation (table/dispatch.h).
 * The files of a format (table/v1/) do the work; the resource manager of logical replication is next to this one (table/wal.c).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "core/catname.h"
#include "core/check.h"
#include "core/inspect.h"
#include "core/meta_peek.h"
#include "core/ops.h"
#include "core/sql/sql.h"
#include "table/dispatch.h"

PG_FUNCTION_INFO_V1(sift_table_handler);

static const SiftTableFormatOps *const format_ops[] = {[1] = &sift_tbl_v1_ops,[2] = &sift_tbl_v2_ops};

const SiftTableFormatOps *
sift_tbl_ops_of_major(uint16 major)
{
	if (major >= lengthof(format_ops) || format_ops[major] == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_table has no engine for format major %u in pg_sift %s", major, SIFT_MODULE_VERSION),
						errhint("Install the pg_sift release that wrote the table.")));
	return format_ops[major];
}

const SiftTableFormatOps *
sift_tbl_ops_for_new(void)
{
	return sift_tbl_ops_of_major(sift_new_relation_format_major());
}

/* The relcache entry carries the table once the format has set it up; until then block 0 tells */
const SiftTableFormatOps *
sift_tbl_ops(Relation rel)
{
	if (rel->rd_amcache != NULL)
		return ((SiftTblRelHeader *) rel->rd_amcache)->ops;
	return sift_tbl_ops_of_major(sift_meta_resolve_major(rel));
}

PG_FUNCTION_INFO_V1(sift_table_paths_trg);

/* Whether the relation is a sift_table, from its row in pg_class: the lock it needs is taken at once, and never raised from one that a statement of the session holds already */
static bool
is_sift_table(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		ours;

	if (!HeapTupleIsValid(tup))
		return false;
	ours = ((Form_pg_class) GETSTRUCT(tup))->relam == get_table_am_oid("sift_table", true);
	ReleaseSysCache(tup);
	return ours;
}

/* The table a row of sift.table_paths is about to change the declaration of: its format writes out what it holds under the old one, once for the statement's first row of it */
static void
paths_row_changed(HeapTuple row, TupleDesc desc)
{
	bool		isnull;
	Oid			relid = DatumGetObjectId(heap_getattr(row, 1, desc, &isnull));
	Relation	rel;

	if (isnull || !is_sift_table(relid))
		return;
	rel = relation_open(relid, AccessExclusiveLock);
	if (sift_tbl_ops(rel)->paths_changed != NULL)
		sift_tbl_ops(rel)->paths_changed(rel);
	relation_close(rel, NoLock);
}

/*
 * BEFORE ROW on the rows of sift.table_paths, whichever statement changes them (sift.table_set_paths, a rename of a column, a restore, a repair by hand): a table of a format that lays its
 * stored rows out by its declared paths finishes what it holds under the old declaration and replaces its store in every backend.
 */
Datum
sift_table_paths_trg(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	TupleDesc	desc;

	sift_check_catalog_trigger(fcinfo, "sift.table_paths_trg", "sift.table_paths");
	desc = td->tg_relation->rd_att;
	paths_row_changed(td->tg_trigtuple, desc);
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event))
	{
		paths_row_changed(td->tg_newtuple, desc);
		return PointerGetDatum(td->tg_newtuple);
	}
	return PointerGetDatum(td->tg_trigtuple);
}

static const TableAmRoutine sift_tbl_methods;

const TableAmRoutine *
sift_tbl_routine(void)
{
	return &sift_tbl_methods;
}

bool
sift_tbl_is_table(Relation rel)
{
	return rel->rd_tableam == sift_tbl_routine();
}

bool
sift_tbl_is_f2_oid(Oid relid)
{
	Relation	rel = table_open(relid, NoLock);
	bool		f2 = sift_tbl_is_table(rel) && sift_meta_resolve_major(rel) == SIFT_FORMAT2_MAJOR;

	table_close(rel, NoLock);
	return f2;
}

void
sift_tbl_require(Relation rel)
{
	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
}

Relation
sift_tbl_open_verifiable(Oid relid)
{
	Relation	rel;

	sift_require_acl(sift_rel_must_exist(relid), ACL_MAINTAIN);
	rel = table_open(relid, AccessShareLock);
	sift_tbl_require(rel);
	sift_require_acl(relid, ACL_MAINTAIN);
	return rel;
}

void
sift_tbl_check(SiftCheck *ck, Oid relid)
{
	Relation	rel = sift_tbl_open_verifiable(relid);

	if (sift_meta_peek_format(rel).status != SIFT_FMT_EMPTY)
		sift_tbl_ops(rel)->check(ck, rel);
	table_close(rel, AccessShareLock);
}

#define REL_TAM(rel)	(sift_tbl_ops(rel)->tam)
#define SCAN_TAM(scan)	(((SiftTblScanHeader *) (scan))->ops->tam)
#define FETCH_TAM(data)	(((SiftTblFetchHeader *) (data))->ops->tam)

static const TupleTableSlotOps *
tbl_slot_callbacks(Relation rel)
{
	return REL_TAM(rel)->slot_callbacks(rel);
}

static TableScanDesc
tbl_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
	return REL_TAM(rel)->scan_begin(rel, snapshot, nkeys, key, pscan, flags);
}

static void
tbl_scan_end(TableScanDesc scan)
{
	SCAN_TAM(scan)->scan_end(scan);
}

static void
tbl_scan_rescan(TableScanDesc scan, struct ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	SCAN_TAM(scan)->scan_rescan(scan, key, set_params, allow_strat, allow_sync, allow_pagemode);
}

static bool
tbl_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	return SCAN_TAM(scan)->scan_getnextslot(scan, direction, slot);
}

static void
tbl_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid)
{
	SCAN_TAM(scan)->scan_set_tidrange(scan, mintid, maxtid);
}

static bool
tbl_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	return SCAN_TAM(scan)->scan_getnextslot_tidrange(scan, direction, slot);
}

static Size
tbl_parallelscan_estimate(Relation rel)
{
	return REL_TAM(rel)->parallelscan_estimate(rel);
}

static Size
tbl_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	return REL_TAM(rel)->parallelscan_initialize(rel, pscan);
}

static void
tbl_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	REL_TAM(rel)->parallelscan_reinitialize(rel, pscan);
}

static IndexFetchTableData *
tbl_index_fetch_begin(Relation rel SIFT_TAM_FLAGS_PARAM)
{
	return REL_TAM(rel)->index_fetch_begin(rel SIFT_TAM_FLAGS_ARG);
}

static void
tbl_index_fetch_reset(IndexFetchTableData *data)
{
	FETCH_TAM(data)->index_fetch_reset(data);
}

static void
tbl_index_fetch_end(IndexFetchTableData *data)
{
	FETCH_TAM(data)->index_fetch_end(data);
}

static bool
tbl_index_fetch_tuple(IndexFetchTableData *data, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
	return FETCH_TAM(data)->index_fetch_tuple(data, tid, snapshot, slot, call_again, all_dead);
}

static bool
tbl_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	return REL_TAM(rel)->tuple_fetch_row_version(rel, tid, snapshot, slot);
}

static bool
tbl_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return SCAN_TAM(scan)->tuple_tid_valid(scan, tid);
}

static void
tbl_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	SCAN_TAM(scan)->tuple_get_latest_tid(scan, tid);
}

static bool
tbl_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	return REL_TAM(rel)->tuple_satisfies_snapshot(rel, slot, snapshot);
}

static TransactionId
tbl_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	return REL_TAM(rel)->index_delete_tuples(rel, delstate);
}

static void
tbl_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	REL_TAM(rel)->tuple_insert(rel, slot, cid, options, bistate);
}

static void
tbl_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, BulkInsertState bistate, uint32 specToken)
{
	REL_TAM(rel)->tuple_insert_speculative(rel, slot, cid, options, bistate, specToken);
}

static void
tbl_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
	REL_TAM(rel)->tuple_complete_speculative(rel, slot, specToken, succeeded);
}

static void
tbl_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	REL_TAM(rel)->multi_insert(rel, slots, nslots, cid, options, bistate);
}

#if PG_VERSION_NUM >= 190000
static TM_Result
tbl_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd)
{
	return REL_TAM(rel)->tuple_delete(rel, tid, cid, options, snapshot, crosscheck, wait, tmfd);
}

static TM_Result
tbl_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait,
				 TM_FailureData *tmfd, LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	return REL_TAM(rel)->tuple_update(rel, otid, slot, cid, options, snapshot, crosscheck, wait, tmfd, lockmode, update_indexes);
}
#else
static TM_Result
tbl_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
	return REL_TAM(rel)->tuple_delete(rel, tid, cid, snapshot, crosscheck, wait, tmfd, changingPart);
}

static TM_Result
tbl_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
				 TM_FailureData *tmfd, LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	return REL_TAM(rel)->tuple_update(rel, otid, slot, cid, snapshot, crosscheck, wait, tmfd, lockmode, update_indexes);
}
#endif

static TM_Result
tbl_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
			   uint8 flags, TM_FailureData *tmfd)
{
	return REL_TAM(rel)->tuple_lock(rel, tid, snapshot, slot, cid, mode, wait_policy, flags, tmfd);
}

static void
tbl_finish_bulk_insert(Relation rel, SiftTamOptions options)
{
	REL_TAM(rel)->finish_bulk_insert(rel, options);
}

/* The storage of a new relation is the one of the format a new relation takes: the old one may be of another */
/* The new heap of a rewrite keeps the format of the table it replaces unless new relations take a newer one: a Format 1 table is rewritten as Format 2.0, never the reverse */
static const SiftTableFormatOps *
new_file_ops(Relation rel)
{
	Oid			from = sift_tbl_rewrite_source(rel);
	uint16		major = sift_new_relation_format_major();
	Relation	old;

	if (!OidIsValid(from))
		return sift_tbl_ops_of_major(major);
	old = relation_open(from, NoLock);
	major = Max(major, sift_tbl_ops(old)->format_major);
	relation_close(old, NoLock);
	return sift_tbl_ops_of_major(major);
}

static void
tbl_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	new_file_ops(rel)->tam->relation_set_new_filelocator(rel, newrlocator, persistence, freezeXid, minmulti);
}

static void
tbl_relation_nontransactional_truncate(Relation rel)
{
	REL_TAM(rel)->relation_nontransactional_truncate(rel);
}

static void
tbl_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	REL_TAM(rel)->relation_copy_data(rel, newrlocator);
}

/* The rows are written by the format of the new relation, which reads a table of an older format through its scan */
static void
tbl_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, TransactionId OldestXmin,
							  SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed,
							  double *tups_recently_dead)
{
	REL_TAM(NewTable)->relation_copy_for_cluster(OldTable, NewTable, OldIndex, use_sort, OldestXmin, SIFT_TAM_CLUSTER_SNAPSHOT_ARG xid_cutoff,
												 multi_cutoff, num_tuples, tups_vacuumed, tups_recently_dead);
}

static void
tbl_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy)
{
	REL_TAM(rel)->relation_vacuum(rel, params, bstrategy);
}

static bool
tbl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return SCAN_TAM(scan)->scan_analyze_next_block(scan, stream);
}

static bool
tbl_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows, double *deadrows, TupleTableSlot *slot)
{
	return SCAN_TAM(scan)->scan_analyze_next_tuple(scan, SIFT_TAM_ANALYZE_XMIN_ARG liverows, deadrows, slot);
}

static double
tbl_index_build_range_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, bool allow_sync, bool anyvisible, bool progress,
						   BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	return REL_TAM(table_rel)->index_build_range_scan(table_rel, index_rel, index_info, allow_sync, anyvisible, progress, start_blockno, numblocks,
													  callback, callback_state, scan);
}

static void
tbl_index_validate_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, Snapshot snapshot, struct ValidateIndexState *state)
{
	REL_TAM(table_rel)->index_validate_scan(table_rel, index_rel, index_info, snapshot, state);
}

static uint64
tbl_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

static bool
tbl_relation_needs_toast_table(Relation rel)
{
	return false;
}

static void
tbl_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
	REL_TAM(rel)->relation_estimate_size(rel, attr_widths, pages, tuples, allvisfrac);
}

/*
 * Bitmap heap scans from 18 on: before 18 the executor prefetches the blocks of the bitmap itself, and a TID of a sift_table names a virtual block that may be past the end of its file
 * ("previous segment is only N blocks"). Without scan_bitmap_next_block the planner offers no bitmap scan of the table.
 */
#if PG_VERSION_NUM >= 180000
static bool
tbl_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages, uint64 *exact_pages)
{
	return SCAN_TAM(scan)->scan_bitmap_next_tuple(scan, slot, recheck, lossy_pages, exact_pages);
}
#endif

static bool
tbl_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate)
{
	return SCAN_TAM(scan)->scan_sample_next_block(scan, scanstate);
}

static bool
tbl_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate, TupleTableSlot *slot)
{
	return SCAN_TAM(scan)->scan_sample_next_tuple(scan, scanstate, slot);
}

static const TableAmRoutine sift_tbl_methods = {
	.type = T_TableAmRoutine,
	.slot_callbacks = tbl_slot_callbacks,
	.scan_begin = tbl_scan_begin,
	.scan_end = tbl_scan_end,
	.scan_rescan = tbl_scan_rescan,
	.scan_getnextslot = tbl_scan_getnextslot,
	.scan_set_tidrange = tbl_scan_set_tidrange,
	.scan_getnextslot_tidrange = tbl_scan_getnextslot_tidrange,
	.parallelscan_estimate = tbl_parallelscan_estimate,
	.parallelscan_initialize = tbl_parallelscan_initialize,
	.parallelscan_reinitialize = tbl_parallelscan_reinitialize,
	.index_fetch_begin = tbl_index_fetch_begin,
	.index_fetch_reset = tbl_index_fetch_reset,
	.index_fetch_end = tbl_index_fetch_end,
	.index_fetch_tuple = tbl_index_fetch_tuple,
	.tuple_fetch_row_version = tbl_tuple_fetch_row_version,
	.tuple_tid_valid = tbl_tuple_tid_valid,
	.tuple_get_latest_tid = tbl_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = tbl_tuple_satisfies_snapshot,
	.index_delete_tuples = tbl_index_delete_tuples,
	.tuple_insert = tbl_tuple_insert,
	.tuple_insert_speculative = tbl_tuple_insert_speculative,
	.tuple_complete_speculative = tbl_tuple_complete_speculative,
	.multi_insert = tbl_multi_insert,
	.tuple_delete = tbl_tuple_delete,
	.tuple_update = tbl_tuple_update,
	.tuple_lock = tbl_tuple_lock,
	.finish_bulk_insert = tbl_finish_bulk_insert,
	.relation_set_new_filelocator = tbl_relation_set_new_filelocator,
	.relation_nontransactional_truncate = tbl_relation_nontransactional_truncate,
	.relation_copy_data = tbl_relation_copy_data,
	.relation_copy_for_cluster = tbl_relation_copy_for_cluster,
	.relation_vacuum = tbl_relation_vacuum,
	.scan_analyze_next_block = tbl_scan_analyze_next_block,
	.scan_analyze_next_tuple = tbl_scan_analyze_next_tuple,
	.index_build_range_scan = tbl_index_build_range_scan,
	.index_validate_scan = tbl_index_validate_scan,
	.relation_size = tbl_relation_size,
	.relation_needs_toast_table = tbl_relation_needs_toast_table,
	.relation_estimate_size = tbl_relation_estimate_size,
#if PG_VERSION_NUM >= 180000
	.scan_bitmap_next_tuple = tbl_scan_bitmap_next_tuple,
#endif
	.scan_sample_next_block = tbl_scan_sample_next_block,
	.scan_sample_next_tuple = tbl_scan_sample_next_tuple,
};

Datum
sift_table_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&sift_tbl_methods);
}
