/*
 * table/v1/am.c - the Format 1 table access method sift_table: DML, relation lifecycle (docs/COL-TAM.md section 4).
 *
 * Row locking protocol of DELETE, UPDATE and SELECT FOR ...: LockTuple on the TID, the tombstone log brought up to date, the
 * deletion state of the row decided (sift_tbl_del_state), then the tombstone appended, all under the tuple lock, which is
 * released at the end of the call as heap_delete does. A transaction in progress that modified or locked the row is waited for
 * with XactLockTableWait outside the tuple lock.
 */
#include "postgres.h"

#include <math.h>


#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "table/v1/table.h"

bool		sift_tbl_inline_promote = true;
bool		sift_tbl_hot_update = true;
int			sift_tbl_backpressure = 0;
char	   *sift_tbl_compression_spec = "";

static void
define_gucs(void)
{
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.table_max_segments", "Test only: the directory entries a sift_table may use.", NULL,
							&sift_tbl_max_segments, SIFT_DIR_POLICY_MAX, 8, SIFT_DIR_POLICY_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_vblock_floor", "Test only: the first virtual block a new write buffer of a sift_table may take.", NULL,
							&sift_tbl_vblock_floor, 0, 0, 1 << 30, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_init_gate", "Test only: the first write of a sift_table waits for the advisory lock of this number before it creates the tombstone segment (0 = no gate).", NULL,
							&sift_tbl_init_gate, 0, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_seal_gate", "Test only: the writer that closed a write buffer of a sift_table waits for the advisory lock of this number before it flags the pages sealed (0 = no gate).", NULL,
							&sift_tbl_seal_gate, 0, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
#endif
}

static const TupleTableSlotOps *
tbl_slot_callbacks(Relation rel)
{
	return sift_tbl_slot_ops();
}

static TransactionId
insert_xmin(SiftTamOptions options)
{
	return (options & TABLE_INSERT_FROZEN) ? FrozenTransactionId : GetCurrentTransactionId();
}

static void
insert_one(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, uint32 token)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	MinimalTuple mtup;
	bool		logical,
				free_mtup;

	sift_tbl_ensure_init(rel);
	sift_tbl_layout_ready(rel, st);
	logical = sift_tbl_check_publication(rel);
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	mtup = sift_tbl_detoast_slot_tuple(rel, slot, &free_mtup);
	sift_tbl_append(rel, st, mtup, insert_xmin(options), cid, token, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	if (logical && token == 0)
		sift_tbl_log_change(rel, SIFT_TBL_OP_INSERT, mtup, NULL);
	if (free_mtup)
		heap_free_minimal_tuple(mtup);
	pgstat_count_heap_insert(rel, 1);
}

/* A row of COPY, CREATE TABLE AS or REFRESH MATERIALIZED VIEW (they pass a bulk insert state) goes into a column group when sift.table_bulk_groups says so */
static bool
insert_bulk(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	SiftTblRel *st;
	Datum	   *values;

	if (bistate == NULL)
		return false;
	st = sift_tbl_rel(rel);
	if (!sift_tbl_opt_bool(st, SIFT_TBLOPT_BULK_GROUPS))
		return false;
	sift_tbl_ensure_init(rel);
	sift_tbl_layout_ready(rel, st);
	if (!sift_tbl_load_wanted(rel, st, (options & TABLE_INSERT_FROZEN) != 0))
		return false;
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	values = sift_tbl_slot_values(RelationGetDescr(rel), slot);
	sift_tbl_load_row(rel, st, values, slot->tts_isnull, cid, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	sift_tbl_slot_values_free(RelationGetDescr(rel), slot, values);
	pgstat_count_heap_insert(rel, 1);
	return true;
}

static void
tbl_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	if (!insert_bulk(rel, slot, cid, options, bistate))
		insert_one(rel, slot, cid, options, 0);
}

static void
tbl_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, BulkInsertState bistate,
							 uint32 specToken)
{
	insert_one(rel, slot, cid, options, specToken);
}

static void
tbl_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	/* a confirmed insertion just stops being speculative (a row that stays flagged would keep its write buffer from being promoted); a killed one must never be seen by anyone */
	sift_tbl_rec_flag(rel, st, &slot->tts_tid, succeeded ? 0 : SIFT_TBL_REC_DEAD, succeeded ? SIFT_TBL_REC_SPEC : 0);
	if (!succeeded)
		pgstat_count_heap_delete(rel);
	else if (sift_tbl_check_publication(rel))
	{
		bool		free_mtup;
		MinimalTuple mtup = sift_tbl_detoast_slot_tuple(rel, slot, &free_mtup);

		sift_tbl_log_change(rel, SIFT_TBL_OP_INSERT, mtup, NULL);
		if (free_mtup)
			heap_free_minimal_tuple(mtup);
	}
}

static void
tbl_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	sift_tbl_load_from_copy = true;
	for (int i = 0; i < nslots; i++)
		if (!insert_bulk(rel, slots[i], cid, options, bistate))
			insert_one(rel, slots[i], cid, options, 0);
	sift_tbl_load_from_copy = false;
}

/*
 * Decides whether the version at tid may be deleted, updated or locked now. TM_Ok: the tuple lock is held, *del is the state
 * of the row, the caller appends its tombstone and releases it. Anything else: the lock is released and tmfd filled.
 */
static TM_Result
tomb_claim(Relation rel, SiftTblRel *st, ItemPointer tid, CommandId cid, bool wait, LockWaitPolicy policy,
		   bool locking, TM_FailureData *tmfd, SiftTblDel *del)
{
	sift_tbl_auto_defrag_tick(rel, st);
	for (;;)
	{
		SiftTblRowVer rv;
		TransactionId waitxid;

		LockTuple(rel, tid, ExclusiveLock);
		sift_tbl_refresh(rel, st);
		sift_tomb_sync(rel, &st->tomb);
		if (!sift_tbl_fetch_lazy(rel, st, tid, &rv))
		{
			UnlockTuple(rel, tid, ExclusiveLock);
			if (locking)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = InvalidTransactionId;
				return TM_Deleted;
			}
			SIFT_INVARIANT(false, "attempted to modify invisible tuple (%u,%u) of \"%s\"", ItemPointerGetBlockNumberNoCheck(tid),
						   ItemPointerGetOffsetNumberNoCheck(tid), RelationGetRelationName(rel));
		}
		sift_tbl_free_row(&rv);
		sift_tbl_del_state(st, rv.key, del);
		tmfd->ctid = *tid;
		tmfd->xmax = del->xid;
		tmfd->cmax = del->cid;
		switch (del->state)
		{
			case SIFT_TBL_DEL_SELF:
				UnlockTuple(rel, tid, ExclusiveLock);
				if (del->cid >= cid || locking)
					return TM_SelfModified;
				SIFT_INVARIANT(false, "attempted to modify a tuple deleted by an earlier command of \"%s\"", RelationGetRelationName(rel));
				break;
			case SIFT_TBL_DEL_DELETED:
				UnlockTuple(rel, tid, ExclusiveLock);
				return TM_Deleted;
			case SIFT_TBL_DEL_UPDATED:
				UnlockTuple(rel, tid, ExclusiveLock);
				sift_key_to_tid(del->succ, &tmfd->ctid);
				return TM_Updated;
			case SIFT_TBL_DEL_INPROGRESS:
				waitxid = del->xid;
				break;
			default:
				waitxid = del->lockxid;	/* another transaction's row lock excludes us as well (docs/COL-TAM.md section 1) */
				break;
		}
		if (!TransactionIdIsValid(waitxid))
			return TM_Ok;
		UnlockTuple(rel, tid, ExclusiveLock);
		tmfd->xmax = waitxid;
		if (locking && policy == LockWaitSkip)
			return TM_WouldBlock;
		if (locking && policy == LockWaitError)
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not obtain lock on row in relation \"%s\"", RelationGetRelationName(rel))));
		if (!wait)
			return TM_BeingModified;
		XactLockTableWait(waitxid, rel, tid, locking ? XLTW_Lock : XLTW_Delete);
	}
}

/* The logical record of a change to the row at otid; its current version is the old row (replica identity) */
static void
log_old_row(Relation rel, SiftTblRel *st, SiftTblOp op, ItemPointer otid, MinimalTuple newrow)
{
	SiftTblRowVer rv;

	if (!sift_tbl_fetch_row(rel, st, otid, &rv))
		SIFT_TBL_RETRY("could not read the old row of \"%s\" for logical decoding", RelationGetRelationName(rel));
	sift_tbl_log_change(rel, op, newrow, rv.tuple);
	heap_freetuple(rv.tuple);
}

static TM_Result
tbl_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
				 TM_FailureData *tmfd, bool changingPart)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftTblDel	del;
	TM_Result	result;
	uint64		key = sift_key_from_tid(tid);
	bool		logical = sift_tbl_check_publication(rel);

	result = tomb_claim(rel, st, tid, cid, wait, LockWaitBlock, false, tmfd, &del);
	if (result != TM_Ok)
		return result;
	if (crosscheck != InvalidSnapshot)
	{
		SiftTblRowVer rv;

		if (sift_tbl_fetch_lazy(rel, st, tid, &rv))
		{
			if (!sift_tbl_visible(rel, st, crosscheck, &rv))
				result = TM_Updated;
			sift_tbl_free_row(&rv);
		}
	}
	if (result == TM_Ok)
	{
		uint64		moved;
		ItemPointerData mp;

		ItemPointerSetMovedPartitions(&mp);
		moved = sift_key_from_tid(&mp);
		sift_tomb_append(rel, &st->tomb, key, GetCurrentTransactionId(), cid,
							 changingPart ? SIFT_TOMB_UPDATE : SIFT_TOMB_DELETE, 0, changingPart ? moved : 0);
		CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
		if (logical)
			log_old_row(rel, st, SIFT_TBL_OP_DELETE, tid, NULL);
		pgstat_count_heap_delete(rel);
	}
	UnlockTuple(rel, tid, ExclusiveLock);
	return result;
}

/*
 * Does the new version keep every column an index reads, as binary datums (the heap's test): then no index gets an entry for it, its tombstone is a HOT
 * one and the old version's entries lead to it. Not with a summarizing index, which wants the new
 * version's block, nor on a table without index (nothing to save). The sift indexes are no exception: they read the columns they name, and the visibility
 * of their TIDs follows the chain (sift_tbl_filter_page).
 */
static bool
index_columns_kept(Relation rel, SiftTblRel *st, ItemPointer otid, MinimalTuple mtup)
{
	Bitmapset  *attrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_HOT_BLOCKING);
	TupleDesc	desc = RelationGetDescr(rel);
	HeapTupleData new = {.t_len = mtup->t_len + MINIMAL_TUPLE_OFFSET,.t_data = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET)};
	SiftTblRowVer rv;
	bool		same = true;

	if (!sift_tbl_opt_bool(st, SIFT_TBLOPT_HOT_UPDATE) || bms_is_empty(attrs) || !bms_is_empty(RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_SUMMARIZED)) ||
		!sift_tbl_fetch_row(rel, st, otid, &rv))
		return false;
	for (int attr = -1; same && (attr = bms_next_member(attrs, attr)) >= 0;)
	{
		AttrNumber	a = attr + FirstLowInvalidHeapAttributeNumber;
		Form_pg_attribute att;
		bool		onull,
					nnull;
		Datum		old,
					fresh;

		if (a <= 0)
		{
			same = false;		/* a system column or the whole row */
			break;
		}
		att = TupleDescAttr(desc, a - 1);
		old = heap_getattr(rv.tuple, a, desc, &onull);
		fresh = heap_getattr(&new, a, desc, &nnull);
		same = onull == nnull && (onull || datumIsEqual(old, fresh, att->attbyval, att->attlen));
	}
	sift_tbl_free_row(&rv);
	return same;
}

static TM_Result
tbl_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot,
				 Snapshot crosscheck, bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
				 TU_UpdateIndexes *update_indexes)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftTblDel	del;
	TM_Result	result;
	bool		logical = sift_tbl_check_publication(rel);
	/* the caller may pass the address of the slot's tid, which the append of the new version rewrites: the claim, its unlock and the tombstone name the old one */
	ItemPointerData old = *otid;

	*lockmode = LockTupleExclusive;
	*update_indexes = TU_All;
	result = tomb_claim(rel, st, &old, cid, wait, LockWaitBlock, false, tmfd, &del);
	if (result != TM_Ok)
		return result;
	if (crosscheck != InvalidSnapshot)
	{
		SiftTblRowVer rv;

		if (sift_tbl_fetch_lazy(rel, st, &old, &rv))
		{
			if (!sift_tbl_visible(rel, st, crosscheck, &rv))
				result = TM_Updated;
			sift_tbl_free_row(&rv);
		}
	}
	if (result == TM_Ok)
	{
		bool		free_mtup;
		MinimalTuple mtup = sift_tbl_detoast_slot_tuple(rel, slot, &free_mtup);
		bool		hot = index_columns_kept(rel, st, &old, mtup);

		sift_tbl_append(rel, st, mtup, GetCurrentTransactionId(), cid, 0, &slot->tts_tid);
		slot->tts_tableOid = RelationGetRelid(rel);
		if (logical)
			log_old_row(rel, st, SIFT_TBL_OP_UPDATE, &old, mtup);
		if (free_mtup)
			heap_free_minimal_tuple(mtup);
		sift_tomb_append(rel, &st->tomb, sift_key_from_tid(&old), GetCurrentTransactionId(), cid, SIFT_TOMB_UPDATE,
							 hot ? SIFT_TOMB_HOT : 0, sift_key_from_tid(&slot->tts_tid));
		CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
		if (hot)
			*update_indexes = TU_None;
		pgstat_count_heap_update(rel, hot, false);
	}
	UnlockTuple(rel, &old, ExclusiveLock);
	return result;
}

SIFT_TAM_DELETE_ADAPTER(tbl_tuple_delete)
SIFT_TAM_UPDATE_ADAPTER(tbl_tuple_update)

static TM_Result
tbl_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid,
			   LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	tmfd->traversed = false;
	for (;;)
	{
		SiftTblDel	del;
		SiftTblRowVer rv;
		TM_Result	result = tomb_claim(rel, st, tid, cid, true, wait_policy, true, tmfd, &del);

		if (result == TM_Updated && (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
		{
			if (ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent update")));
			*tid = tmfd->ctid;
			tmfd->traversed = true;
			continue;
		}
		if (result != TM_Ok)
			return result;
		if (!del.self_locked)
			sift_tomb_append(rel, &st->tomb, sift_key_from_tid(tid), GetCurrentTransactionId(), cid, SIFT_TOMB_LOCK, 0, 0);
		UnlockTuple(rel, tid, ExclusiveLock);
		if (!sift_tbl_fetch_row(rel, st, tid, &rv))
			SIFT_TBL_RETRY("could not read the locked tuple of \"%s\"", RelationGetRelationName(rel));
		sift_tbl_store(slot, rv.tuple);
		return TM_Ok;
	}
}

static void
tbl_finish_bulk_insert(Relation rel, SiftTamOptions options)
{
	sift_tbl_load_finish(rel);
}

static void
tbl_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence,
								 TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	if (BLCKSZ != 8192)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_table needs BLCKSZ 8192"),
				 errdetail("This server has BLCKSZ %d.", BLCKSZ),
				 errhint("Use a PostgreSQL build with the default 8 kB block size.")));
	sift_tbl_layout_check(rel);
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	srel = RelationCreateStorage(*newrlocator, persistence, true);
	/* the layout is written by the first writer (sift_tbl_ensure_init); an unlogged table's empty init fork resets to empty */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}
	smgrclose(srel);
}

static void
tbl_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
	sift_tbl_rel(rel)->invalid = true;
}

static void
tbl_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel = smgropen(*newrlocator, rel->rd_backend);

	RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);
	/* the copy reads the files, not the buffers */
	FlushRelationBuffers(rel);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM, rel->rd_rel->relpersistence);
	for (ForkNumber fork = MAIN_FORKNUM + 1; fork <= MAX_FORKNUM; fork++)
		if (smgrexists(RelationGetSmgr(rel), fork))
		{
			smgrcreate(dstrel, fork, false);
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED && fork == INIT_FORKNUM))
				log_smgrcreate(newrlocator, fork);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, fork, rel->rd_rel->relpersistence);
		}
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/*
 * A scan of a sift_table builds every row it returns from the columns: 1.7 times the instructions of a heap scan of the same rows
 * (the store benchmark, 1M orders, a grouped scan of two columns), whatever the size of the compressed file. The planner prices a scan
 * by pages, so it gets the pages of the rows in a heap, twice: a tenth of that (the blocks of the compressed columns) made a
 * sequential scan look a tenth as dear, and it beat the plans that read the columns instead (SiftAgg), which build no rows.
 */
#define TBL_SCAN_WORK	2.0

static void
tbl_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
	int32		overhead = MAXALIGN(sizeof(SiftTblRec)) + sizeof(uint16);
	double		scan_pages;

	table_block_relation_estimate_size(rel, attr_widths, pages, tuples, allvisfrac, overhead, BLCKSZ - SizeOfPageHeaderData);
	scan_pages = ceil(*tuples * (get_rel_data_width(rel, attr_widths) + overhead) / (BLCKSZ - SizeOfPageHeaderData) * TBL_SCAN_WORK);
	*pages = Max(*pages, (BlockNumber) Min(scan_pages, (double) MaxBlockNumber));
	*allvisfrac = 0;
}

void
sift_tbl_v1_init(void)
{
	define_gucs();
	sift_tbl_slot_init();
}

const TableAmRoutine sift_tbl_v1_methods = {
	.type = T_TableAmRoutine,
	.slot_callbacks = tbl_slot_callbacks,
	.scan_begin = sift_tbl_scan_begin,
	.scan_end = sift_tbl_scan_end,
	.scan_rescan = sift_tbl_scan_rescan,
	.scan_getnextslot = sift_tbl_scan_getnextslot,
	.scan_set_tidrange = sift_tbl_scan_set_tidrange,
	.scan_getnextslot_tidrange = sift_tbl_scan_getnextslot,
	.parallelscan_estimate = sift_tbl_parallelscan_estimate,
	.parallelscan_initialize = sift_tbl_parallelscan_initialize,
	.parallelscan_reinitialize = sift_tbl_parallelscan_reinitialize,
	.index_fetch_begin = sift_tbl_index_fetch_begin,
	.index_fetch_reset = sift_tbl_index_fetch_reset,
	.index_fetch_end = sift_tbl_index_fetch_end,
	.index_fetch_tuple = sift_tbl_index_fetch_tuple,
	.tuple_fetch_row_version = sift_tbl_tuple_fetch_row_version,
	.tuple_tid_valid = sift_tbl_tuple_tid_valid,
	.tuple_get_latest_tid = sift_tbl_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = sift_tbl_tuple_satisfies_snapshot,
	.index_delete_tuples = sift_tbl_index_delete_tuples,
	.tuple_insert = tbl_tuple_insert,
	.tuple_insert_speculative = tbl_tuple_insert_speculative,
	.tuple_complete_speculative = tbl_tuple_complete_speculative,
	.multi_insert = tbl_multi_insert,
	.tuple_delete = SIFT_TAM(tbl_tuple_delete),
	.tuple_update = SIFT_TAM(tbl_tuple_update),
	.tuple_lock = tbl_tuple_lock,
	.finish_bulk_insert = tbl_finish_bulk_insert,
	.relation_set_new_filelocator = tbl_relation_set_new_filelocator,
	.relation_nontransactional_truncate = tbl_relation_nontransactional_truncate,
	.relation_copy_data = tbl_relation_copy_data,
	.relation_copy_for_cluster = sift_tbl_relation_copy_for_cluster,
	.relation_vacuum = sift_tbl_relation_vacuum,
	.scan_analyze_next_block = sift_tbl_scan_analyze_next_block,
	.scan_analyze_next_tuple = sift_tbl_scan_analyze_next_tuple,
	.index_build_range_scan = sift_tbl_index_build_range_scan,
	.index_validate_scan = sift_tbl_index_validate_scan,
	.relation_estimate_size = tbl_relation_estimate_size,
#if PG_VERSION_NUM >= 180000
	.scan_bitmap_next_tuple = sift_tbl_scan_bitmap_next_tuple,
#endif
	.scan_sample_next_block = sift_tbl_scan_sample_next_block,
	.scan_sample_next_tuple = sift_tbl_scan_sample_next_tuple,
};
