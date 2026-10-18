/*
 * sift_table.c - the table access method sift_table: handler, DML, relation lifecycle (docs/COL-TAM.md section 4).
 *
 * Row locking protocol of DELETE, UPDATE and SELECT FOR ...: LockTuple on the TID, the tombstone log brought up to date, the
 * deletion state of the row decided (sift_tbl_del_state), then the tombstone appended, all under the tuple lock, which is
 * released at the end of the call as heap_delete does. A transaction in progress that modified or locked the row is waited for
 * with XactLockTableWait outside the tuple lock.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "access/detoast.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_publication.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "sift_col_am.h"
#include "sift_merge.h"
#include "sift_table.h"

PG_FUNCTION_INFO_V1(sift_table_handler);

bool		sift_tbl_inline_seal = true;
char	   *sift_tbl_compression_spec = NULL;

static void
check_compression(const char *spec)
{
	if (spec != NULL)
		sift_col_validate_compression(spec);
}

static bool
compression_check_hook(char **newval, void **extra, GucSource source)
{
	check_compression(*newval);
	return true;
}

void
sift_tbl_define_gucs(void)
{
	DefineCustomIntVariable("sift.table_delta_limit",
							"Size of a sift_table write segment (row format) before it is closed, to be sealed into columns.",
							"Rounded down to whole superblocks of 2 MB; a row has to fit one segment.",
							&sift_tbl_delta_limit, 4096, 2048, 8192,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_inline_seal",
							 "Seals the closed write segments of a sift_table into columns while rows are inserted (VACUUM always does).",
							 NULL, &sift_tbl_inline_seal, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.table_compression",
							   "Compression of the column segments a sift_table writes: auto, none, lz4, zstd or encodings; per column: 'auto; col=zstd:9'.",
							   NULL, &sift_tbl_compression_spec, "", PGC_USERSET, 0, compression_check_hook, NULL, NULL);
	DefineCustomIntVariable("sift.table_dead_rewrite_pct",
							"Percent of dead rows above which VACUUM rewrites a column segment of a sift_table alone to drop them.",
							"100 never rewrites.", &sift_tbl_dead_rewrite_pct, SIFT_MERGE_DEAD_PCT, 1, 100,
							PGC_USERSET, 0, NULL, NULL, NULL);
}

static const TableAmRoutine sift_tbl_methods;

const TableAmRoutine *
sift_tbl_routine(void)
{
	return &sift_tbl_methods;
}

static const TupleTableSlotOps *
tbl_slot_callbacks(Relation rel)
{
	return sift_tbl_slot_ops();
}

/* ---------- rows ---------- */

/* The row of slot as a MinimalTuple with every external TOAST value expanded (no TOAST table exists for this AM) */
static MinimalTuple
form_mtuple(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			natts = desc->natts;
	Datum	   *values;
	MinimalTuple mtup;

	slot_getallattrs(slot);
	values = palloc(natts * sizeof(Datum));
	memcpy(values, slot->tts_values, natts * sizeof(Datum));
	for (int i = 0; i < natts; i++)
		if (!slot->tts_isnull[i] && TupleDescAttr(desc, i)->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
			values[i] = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(values[i])));
	mtup = sift_tbl_form_minimal_tuple(desc, values, slot->tts_isnull);
	pfree(values);
	return mtup;
}

static TransactionId
insert_xmin(int options)
{
	return (options & TABLE_INSERT_FROZEN) ? FrozenTransactionId : GetCurrentTransactionId();
}

static void
insert_one(Relation rel, TupleTableSlot *slot, CommandId cid, int options, uint32 token)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	MinimalTuple mtup;
	bool		logical;

	sift_tbl_ensure_init(rel);
	sift_tbl_layout_ready(rel, st);
	logical = sift_tbl_check_publication(rel);
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	mtup = form_mtuple(rel, slot);
	sift_tbl_append(rel, st, mtup, insert_xmin(options), cid, token, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	if (logical && token == 0)
		sift_tbl_log_change(rel, SIFT_TBL_OP_INSERT, mtup, NULL);
	heap_free_minimal_tuple(mtup);
	pgstat_count_heap_insert(rel, 1);
}

static void
tbl_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate)
{
	insert_one(rel, slot, cid, options, 0);
}

static void
tbl_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate,
							 uint32 specToken)
{
	insert_one(rel, slot, cid, options, specToken);
}

static void
tbl_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	/* a confirmed insertion just stops being speculative; a killed one must never be seen by anyone */
	sift_tbl_rec_flag(rel, st, &slot->tts_tid, succeeded ? 0 : SIFT_TBL_REC_DEAD);
	if (!succeeded)
		pgstat_count_heap_delete(rel);
	else if (sift_tbl_check_publication(rel))
	{
		MinimalTuple mtup = form_mtuple(rel, slot);

		sift_tbl_log_change(rel, SIFT_TBL_OP_INSERT, mtup, NULL);
		heap_free_minimal_tuple(mtup);
	}
}

static void
tbl_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, int options, BulkInsertState bistate)
{
	for (int i = 0; i < nslots; i++)
		insert_one(rel, slots[i], cid, options, 0);
}

/* ---------- delete, update, lock ---------- */

/*
 * Decides whether the version at tid may be deleted, updated or locked now. TM_Ok: the tuple lock is held, *del is the state
 * of the row, the caller appends its tombstone and releases it. Anything else: the lock is released and tmfd filled.
 */
static TM_Result
tomb_claim(Relation rel, SiftTblRel *st, ItemPointer tid, CommandId cid, bool wait, LockWaitPolicy policy,
		   bool locking, TM_FailureData *tmfd, SiftTblDel *del)
{
	for (;;)
	{
		SiftTblRowVer rv;
		TransactionId waitxid;

		LockTuple(rel, tid, ExclusiveLock);
		sift_tbl_refresh(rel, st);
		sift_tbl_tomb_sync(rel, st);
		if (!sift_tbl_fetch_lazy(rel, st, tid, &rv))
		{
			UnlockTuple(rel, tid, ExclusiveLock);
			if (locking)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = InvalidTransactionId;
				return TM_Deleted;
			}
			elog(ERROR, "attempted to modify invisible tuple (%u,%u) of \"%s\"", ItemPointerGetBlockNumberNoCheck(tid),
				 ItemPointerGetOffsetNumberNoCheck(tid), RelationGetRelationName(rel));
		}
		sift_tbl_free_row(&rv);
		sift_tbl_del_state(st, rv.key, del);
		tmfd->ctid = *tid;
		tmfd->xmax = del->xid;
		tmfd->cmax = del->cid;
		tmfd->traversed = false;
		switch (del->state)
		{
			case SIFT_TBL_DEL_SELF:
				UnlockTuple(rel, tid, ExclusiveLock);
				if (del->cid >= cid || locking)
					return TM_SelfModified;
				elog(ERROR, "attempted to modify a tuple deleted by an earlier command of \"%s\"", RelationGetRelationName(rel));
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
		elog(ERROR, "could not read the old row of \"%s\" for logical decoding", RelationGetRelationName(rel));
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

		CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
		ItemPointerSetMovedPartitions(&mp);
		moved = sift_key_from_tid(&mp);
		sift_tbl_tomb_append(rel, st, key, GetCurrentTransactionId(), cid,
							 changingPart ? SIFT_TBL_TOMB_UPDATE : SIFT_TBL_TOMB_DELETE, changingPart ? moved : 0);
		if (logical)
			log_old_row(rel, st, SIFT_TBL_OP_DELETE, tid, NULL);
		pgstat_count_heap_delete(rel);
	}
	UnlockTuple(rel, tid, ExclusiveLock);
	return result;
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

	*lockmode = LockTupleExclusive;
	*update_indexes = TU_All;
	result = tomb_claim(rel, st, otid, cid, wait, LockWaitBlock, false, tmfd, &del);
	if (result != TM_Ok)
		return result;
	if (crosscheck != InvalidSnapshot)
	{
		SiftTblRowVer rv;

		if (sift_tbl_fetch_lazy(rel, st, otid, &rv))
		{
			if (!sift_tbl_visible(rel, st, crosscheck, &rv))
				result = TM_Updated;
			sift_tbl_free_row(&rv);
		}
	}
	if (result == TM_Ok)
	{
		MinimalTuple mtup = form_mtuple(rel, slot);

		CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
		sift_tbl_append(rel, st, mtup, GetCurrentTransactionId(), cid, 0, &slot->tts_tid);
		slot->tts_tableOid = RelationGetRelid(rel);
		if (logical)
			log_old_row(rel, st, SIFT_TBL_OP_UPDATE, otid, mtup);
		heap_free_minimal_tuple(mtup);
		sift_tbl_tomb_append(rel, st, sift_key_from_tid(otid), GetCurrentTransactionId(), cid, SIFT_TBL_TOMB_UPDATE,
							 sift_key_from_tid(&slot->tts_tid));
		pgstat_count_heap_update(rel, false, false);
	}
	UnlockTuple(rel, otid, ExclusiveLock);
	return result;
}

static TM_Result
tbl_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid,
			   LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	SiftTblRel *st = sift_tbl_rel(rel);

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
			continue;
		}
		if (result != TM_Ok)
			return result;
		if (!del.self_locked)
			sift_tbl_tomb_append(rel, st, sift_key_from_tid(tid), GetCurrentTransactionId(), cid, SIFT_TBL_TOMB_LOCK, 0);
		UnlockTuple(rel, tid, ExclusiveLock);
		if (!sift_tbl_fetch_row(rel, st, tid, &rv))
			elog(ERROR, "could not read the locked tuple of \"%s\"", RelationGetRelationName(rel));
		sift_tbl_store(slot, rv.tuple);
		if (tmfd->traversed)
			tmfd->traversed = true;
		return TM_Ok;
	}
}

static void
tbl_finish_bulk_insert(Relation rel, int options)
{
}

/* ---------- lifecycle ---------- */

static void
tbl_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence,
								 TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	if (BLCKSZ != 8192)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_table needs BLCKSZ 8192")));
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
	table_block_relation_estimate_size(rel, attr_widths, pages, tuples, allvisfrac,
									   MAXALIGN(sizeof(SiftTblRec)) + sizeof(uint16), BLCKSZ - SizeOfPageHeaderData);
	*allvisfrac = 0;
}

/* ---------- handler ---------- */

static const TableAmRoutine sift_tbl_methods = {
	.type = T_TableAmRoutine,
	.slot_callbacks = tbl_slot_callbacks,
	.scan_begin = sift_tbl_scan_begin,
	.scan_end = sift_tbl_scan_end,
	.scan_rescan = sift_tbl_scan_rescan,
	.scan_getnextslot = sift_tbl_scan_getnextslot,
	.scan_set_tidrange = sift_tbl_scan_set_tidrange,
	.scan_getnextslot_tidrange = sift_tbl_scan_getnextslot_tidrange,
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
	.tuple_delete = tbl_tuple_delete,
	.tuple_update = tbl_tuple_update,
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
	.relation_size = tbl_relation_size,
	.relation_needs_toast_table = tbl_relation_needs_toast_table,
	.relation_estimate_size = tbl_relation_estimate_size,
#if PG_VERSION_NUM < 180000
	.scan_bitmap_next_block = sift_tbl_scan_bitmap_next_block,
#endif
	.scan_bitmap_next_tuple = sift_tbl_scan_bitmap_next_tuple,
	.scan_sample_next_block = sift_tbl_scan_sample_next_block,
	.scan_sample_next_tuple = sift_tbl_scan_sample_next_tuple,
};

Datum
sift_table_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&sift_tbl_methods);
}
