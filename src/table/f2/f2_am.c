/*
 * table/f2/f2_am.c - the relation of a sift_table as the table access method asks for it: its slots, its files, TRUNCATE, copies, its size, and the scans that build indexes.
 *
 * A row is a virtual tuple: the scan fills its values from a granule, so the slot is the plain virtual one. The layout of Format 2.0 (block 0, the empty partition directory, the
 * allocator) is written when the file is made, with the file's own locator; a table has no TOAST table, and its visibility map is indexed by the virtual block of a row (table/f2/f2_vm.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xlogutils.h"
#include "catalog/index.h"
#include "catalog/pg_operator.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "core/shm_manifest.h"
#include "core/compat.h"
#include "col/f2/f2_cache.h"
#include "table/f2/f2_gcache.h"
#include "table/f2/f2_slot.h"
#include "table/f2/f2_tbl.h"
#include "table/f2/f2_vm.h"

const TupleTableSlotOps *
sift_tbl_f2_slot_callbacks(Relation rel pg_attribute_unused())
{
	return sift_f2_slot_ops();
}

/* ---- files ---- */

#ifdef SIFT_TESTING
bool		sift_test_headered_strings = false;
#define NEW_TABLE_COMPAT()	(sift_tbl_host_ops.base_compat_flags & ~(sift_test_headered_strings ? SIFT_COMPAT_NATIVE_STRINGS : 0))
#else
#define NEW_TABLE_COMPAT()	(sift_tbl_host_ops.base_compat_flags)
#endif

static void
init_pages(Relation rel)
{
	sift_col_meta_init(rel, SIFT_AM_TABLE, NEW_TABLE_COMPAT(), 0, 0);
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
		sift_col_meta_init_fork(rel, SIFT_AM_TABLE, NEW_TABLE_COMPAT(), 0, 0);
}

void
sift_tbl_f2_init_layout(Relation rel)
{
	init_pages(rel);
}

void
sift_tbl_f2_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	if (BLCKSZ != 8192)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_table needs BLCKSZ 8192"), errdetail("This server has BLCKSZ %d.", BLCKSZ),
						errhint("Use a PostgreSQL build with the default 8 kB block size.")));
	sift_tbl_f2_guard_publication(rel);
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	srel = RelationCreateStorage(*newrlocator, persistence, true);
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}
	smgrclose(srel);
	sift_stage_discard(RelationGetRelid(rel));
	sift_fsm_cache_discard_rel(rel);
	if (persistence != RELPERSISTENCE_TEMP)
	{
		Relation	fake = CreateFakeRelcacheEntry(*newrlocator);

		fake->rd_rel->relpersistence = persistence;
		init_pages(fake);
		FreeFakeRelcacheEntry(fake);
	}
}

/* The caches of the file are keyed by locator and generation, and a table made in the transaction keeps its file through a TRUNCATE: its generations start again, so a granule of the rows before would answer for the new ones */
static void
forget_caches(Relation rel)
{
	sift_plan_cache_forget(&rel->rd_locator);
	sift_f2_gbuf_forget(rel);
	sift_f2_gcache_forget(&rel->rd_locator);
}

void
sift_tbl_f2_relation_nontransactional_truncate(Relation rel)
{
	forget_caches(rel);
	sift_stage_discard(RelationGetRelid(rel));
	sift_fsm_cache_discard_rel(rel);
	sift_tbl_rowlock_drop_rel(&rel->rd_locator);
	sift_shm_manifest_invalidate(&rel->rd_locator, false);
	RelationTruncate(rel, 0);
	init_pages(rel);
	CacheInvalidateRelcache(rel);
}

void
sift_tbl_f2_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel = smgropen(*newrlocator, rel->rd_backend);

	forget_caches(rel);
	sift_fsm_cache_discard_rel(rel);
	sift_tbl_rowlock_drop_rel(&rel->rd_locator);
	sift_shm_manifest_invalidate(&rel->rd_locator, false);
	RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);
	FlushRelationBuffers(rel);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM, rel->rd_rel->relpersistence);
	for (ForkNumber fork = MAIN_FORKNUM + 1; fork <= MAX_FORKNUM; fork++)
		if (smgrexists(RelationGetSmgr(rel), fork))
		{
			smgrcreate(dstrel, fork, false);
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT || (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED && fork == INIT_FORKNUM))
				log_smgrcreate(newrlocator, fork);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, fork, rel->rd_rel->relpersistence);
		}
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

uint64
sift_tbl_f2_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

bool
sift_tbl_f2_relation_needs_toast_table(Relation rel pg_attribute_unused())
{
	return false;
}

/* ---- the planner ---- */

void
sift_tbl_f2_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
	SiftShmIndexMeta peek;
	BlockNumber unused,
				also_unused;
	uint64		rows = 0;

	/* IndexOnlyNext asks the map fork by the vblock of an address: the fraction is that of the vblocks VACUUM found all visible, and 0 where the server cannot log the map */
	sift_tbl_vm_fractions(rel, 0, &unused, &also_unused, allvisfrac);
	if (RelationGetNumberOfBlocks(rel) > 0)
	{
		if (sift_shm_manifest_peek(&rel->rd_locator, &peek))
			rows = peek.total_rows - peek.total_dead;
		else
		{
			SiftColMetaPageData meta = sift_col_meta_read(rel);

			rows = meta.total_rows - meta.total_dead;
		}
	}
	(void) get_rel_data_width(rel, attr_widths);
	*tuples = (double) rows;
	*pages = rows == 0 ? 0 : Max(1, RelationGetNumberOfBlocks(rel));
}

/* ---- rewrites ---- */

double
sift_tbl_f2_rewrite_rows(Relation OldTable, Relation NewTable)
{
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = sift_table_beginscan(OldTable, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(OldTable, NULL);
	SiftTblF2Bulk bulk;
	ItemPointerData tid;
	double		rows = 0;

	sift_tbl_f2_adopt_layout(sift_f2_store_open(NewTable), sift_f2_store_open(OldTable));
	sift_tbl_f2_bulk_begin(&bulk, NewTable, GetCurrentCommandId(true), true);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		sift_tbl_f2_bulk_add(&bulk, slot, &tid);
		rows++;
	}
	sift_tbl_f2_bulk_end(&bulk);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	return rows;
}

void
sift_tbl_f2_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex pg_attribute_unused(), bool use_sort pg_attribute_unused(), TransactionId OldestXmin,
									  SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	*xid_cutoff = OldestXmin;
	*multi_cutoff = GetOldestMultiXactId();
	*tups_vacuumed = *tups_recently_dead = 0;
	*num_tuples = sift_tbl_f2_rewrite_rows(OldTable, NewTable);
}

/* ---- index builds ---- */

double
sift_tbl_f2_index_build_range_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, bool allow_sync, bool anyvisible pg_attribute_unused(), bool progress,
								   BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	ExprState  *predicate = ExecPrepareQual(index_info->ii_Predicate, estate);
	Snapshot	snapshot = InvalidSnapshot;
	bool		own_scan = scan == NULL;
	double		reltuples = 0;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	if (own_scan)
	{
		snapshot = RegisterSnapshot(index_info->ii_Concurrent ? GetTransactionSnapshot() : GetLatestSnapshot());
		scan = table_beginscan_strat(table_rel, snapshot, 0, NULL, true, allow_sync);
		if (start_blockno != 0 || numblocks != InvalidBlockNumber)
		{
			ItemPointerData lo,
						hi;

			ItemPointerSet(&lo, start_blockno, FirstOffsetNumber);
			ItemPointerSet(&hi, numblocks == InvalidBlockNumber ? MaxBlockNumber : start_blockno + numblocks - 1, MaxOffsetNumber);
			sift_tbl_f2_scan_set_tidrange(scan, &lo, &hi);
		}
	}
	else if (scan->rs_snapshot->snapshot_type == SNAPSHOT_ANY)
		/* a parallel build that holds off the writers hands in SnapshotAny, which sees the versions a commit deleted: the index gets the rows the serial build reads. A worker cannot take the latest snapshot */
		sift_tbl_f2_scan_set_snapshot(scan, SnapshotSelf);
	/* a row deleted after the snapshot is not in this index, which a snapshot older than the build must not rely on (indcheckxmin) */
	index_info->ii_BrokenHotChain = true;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		reltuples++;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(index_info, slot, estate, values, isnull);
		callback(index_rel, &slot->tts_tid, values, isnull, true, callback_state);
		if (progress)
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) reltuples);
	}
	table_endscan(scan);
	if (own_scan)
		UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
	return reltuples;
}

/* The visible rows sorted by address and merged with the addresses the index has: one pass of N log N where a scan that probes the index for each row is N squared */
void
sift_tbl_f2_index_validate_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info, Snapshot snapshot, struct ValidateIndexState *state)
{
	Tuplesortstate *mine = tuplesort_begin_datum(INT8OID, Int8LessOperator, InvalidOid, false, maintenance_work_mem, NULL, TUPLESORT_NONE);
	TableScanDesc scan = sift_table_beginscan(table_rel, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	ExprState  *predicate = ExecPrepareQual(index_info->ii_Predicate, estate);
	Datum		idx_val = 0,
				mine_val;
	bool		idx_null,
				mine_null,
				have_idx;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		state->htups += 1;
		tuplesort_putdatum(mine, Int64GetDatum((int64) itemptr_encode(&slot->tts_tid)), false);
	}
	table_endscan(scan);
	tuplesort_performsort(mine);
	have_idx = tuplesort_getdatum(state->tuplesort, true, false, &idx_val, &idx_null, NULL);
	while (tuplesort_getdatum(mine, true, false, &mine_val, &mine_null, NULL))
	{
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();
		while (have_idx && DatumGetInt64(idx_val) < DatumGetInt64(mine_val))
		{
			state->itups += 1;
			have_idx = tuplesort_getdatum(state->tuplesort, true, false, &idx_val, &idx_null, NULL);
		}
		if (have_idx && DatumGetInt64(idx_val) == DatumGetInt64(mine_val))
			continue;
		itemptr_decode(&tid, DatumGetInt64(mine_val));
		if (!table_tuple_fetch_row_version(table_rel, &tid, snapshot, slot))
			continue;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(index_info, slot, estate, values, isnull);
		index_insert(index_rel, values, isnull, &tid, table_rel, index_info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, index_info);
		state->tups_inserted += 1;
	}
	tuplesort_end(mine);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
}
