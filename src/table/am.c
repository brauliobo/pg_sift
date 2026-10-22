/*
 * table/am.c - the table access method sift_table: handler, DML, relation lifecycle (docs/COL-TAM.md section 4).
 *
 * Row locking protocol of DELETE, UPDATE and SELECT FOR ...: LockTuple on the TID, the tombstone log brought up to date, the
 * deletion state of the row decided (sift_tbl_del_state), then the tombstone appended, all under the tuple lock, which is
 * released at the end of the call as heap_delete does. A transaction in progress that modified or locked the row is waited for
 * with XactLockTableWait outside the tuple lock.
 */
#include "postgres.h"

#include <math.h>


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
#include "optimizer/plancat.h"
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

#include "col/am.h"
#include "bm25/merge.h"
#include "table/table.h"

PG_FUNCTION_INFO_V1(sift_table_handler);

bool		sift_tbl_inline_promote = true;
int			sift_tbl_backpressure = 0;
char	   *sift_tbl_compression_spec = NULL;

/* The directory is half full from the first merge on (dir_pressure), so a threshold at or under that is no backpressure */
static bool
backpressure_check_hook(int *newval, void **extra, GucSource source)
{
	if (*newval > 0 && *newval <= 50)
	{
		GUC_check_errdetail("The value is 0 (off) or a percent above 50, where the directory is already filling.");
		return false;
	}
	return true;
}

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
	DefineCustomIntVariable("sift.table_write_buffer_limit",
							"Size of a sift_table write buffer (row format) before it is closed, to be promoted into columns.",
							"Rounded down to whole superblocks of 2 MB; a row has to fit one segment.",
							&sift_tbl_write_buffer_limit, 4096, 2048, 8192,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_wide_rows",
							 "Accepts a row of a sift_table that does not fit one write buffer (up to PostgreSQL's 1 GB), in pages beyond the buffer's table (format 11).",
							 "Off: such a row is refused. On: the write buffer that takes it is marked format 11, which a build that does not know it cannot write to; rows that fit keep the layout of earlier versions.",
							 &sift_tbl_wide_rows, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_inline_promote",
							 "Promotes the closed write buffers of a sift_table into columns while rows are inserted (VACUUM always does).",
							 NULL, &sift_tbl_inline_promote, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_backpressure",
							"Percent of the directory of a sift_table from which a writer that needs a new write buffer first waits for the maintenance lock and merges.",
							"0 (off): a writer waits only when the directory is nearly full. Above 50, the percent of the entries.",
							&sift_tbl_backpressure, 0, 0, 99,
							PGC_USERSET, 0, backpressure_check_hook, NULL, NULL);
	DefineCustomBoolVariable("sift.table_async_promote",
							 "A backend that closes a write buffer of a sift_table asks a background worker to promote it, where it promotes it itself.",
							 "Needs sift.async_maintenance and sift.table_inline_promote. From half of the directory on the writer promotes and merges itself.",
							 &sift_tbl_async_promote, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_bulk_groups",
							 "COPY, CREATE TABLE AS and REFRESH MATERIALIZED VIEW write the rows of a sift_table straight into column groups, with no write buffer to promote.",
							 "Only a table without a unique or exclusion index, a trigger or a foreign key, and not published; other tables keep the write buffers.",
							 &sift_tbl_bulk_groups, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_auto_defrag",
							 "A backend that closes a write buffer of a sift_table asks a background worker to rewrite its mostly dead column segments and give back its free space when it is worth it, without VACUUM.",
							 "Needs sift.async_maintenance. The worker clears no index entry: VACUUM still has to run for those.",
							 &sift_tbl_auto_defrag, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_auto_defrag_free_pct",
							"Percent of the file that is free blocks from which the background pass of a sift_table gathers and returns them.",
							NULL, &sift_tbl_auto_defrag_free_pct, 25, 1, 100,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_auto_defrag_delay",
							"Cost delay of the background pass of a sift_table, in milliseconds: it sleeps this long (at most four times) each time its page accesses add up to vacuum_cost_limit, and after each segment it rewrites.",
							"0 never waits; at most 100, the most vacuum_cost_delay allows.", &sift_tbl_auto_defrag_delay, 10, 0, 100,
							PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_auto_defrag_dead_pct",
							"Percent of the rows of a sift_table that are tombstones from which the background pass is asked for.",
							"The pass rewrites the segments that are more than sift.table_dead_rewrite_pct percent dead; 100 never asks for dead rows.",
							&sift_tbl_auto_defrag_dead_pct, 20, 1, 100, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_auto_defrag_budget",
							"Megabytes of column segments one background pass of a sift_table rewrites by merging at most.",
							"The worker passes again while the budget stopped the last one. 0 is no limit.",
							&sift_tbl_auto_defrag_budget, 128, 0, 1048576, PGC_USERSET, GUC_UNIT_MB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_auto_defrag_max_active",
							"Client backends of the database running a statement above which the background pass of a sift_table waits for a quieter moment.",
							"The worker looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all, and between the merges of a pass; a later request asks again. 0: never waits.",
							&sift_tbl_auto_defrag_max_active, 0, 0, 10000, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_auto_defrag_wal_rate",
							"Kilobytes of WAL per second the background pass of a sift_table may write on average.",
							"After a step that put it ahead of that rate the worker sleeps until the rate is met. A rewritten segment makes every backend that reads it load its dictionaries again, so the rate also bounds how often that happens. 0: no limit.",
							&sift_tbl_auto_defrag_wal_rate, 0, 0, INT_MAX, PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_auto_defrag_coalesce",
							 "The background pass of a sift_table merges the small column groups next to the ones it rewrites into one group.",
							 NULL, &sift_tbl_auto_defrag_coalesce, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.table_auto_defrag_indexes",
							 "The background pass of a sift_table whose dead rows reach sift.table_dead_rewrite_pct is a whole VACUUM, index cleanup and tombstone rewrite included.",
							 "Off: it only clears the dead rows of the column segments and returns space, and the tombstone log and the index entries wait for a VACUUM.",
							 &sift_tbl_auto_defrag_indexes, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.table_compression",
							   "Compression of the column segments a sift_table writes: auto, none, lz4, zstd or encodings; per column: 'auto; col=zstd:9'.",
							   NULL, &sift_tbl_compression_spec, "", PGC_USERSET, 0, compression_check_hook, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.table_max_segments", "Test only: the directory entries a sift_table may use.", NULL,
							&sift_tbl_max_segments, SIFT_MAX_SEGMENTS, 16, SIFT_MAX_SEGMENTS, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_vblock_floor", "Test only: the first virtual block a new write buffer of a sift_table may take.", NULL,
							&sift_tbl_vblock_floor, 0, 0, 1 << 30, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_init_gate", "Test only: the first write of a sift_table waits for the advisory lock of this number before it creates the tombstone segment (0 = no gate).", NULL,
							&sift_tbl_init_gate, 0, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.table_seal_gate", "Test only: the writer that closed a write buffer of a sift_table waits for the advisory lock of this number before it flags the pages sealed (0 = no gate).", NULL,
							&sift_tbl_seal_gate, 0, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
#endif
	DefineCustomIntVariable("sift.table_dead_rewrite_pct",
							"Percent of dead rows above which VACUUM rewrites a column segment of a sift_table alone to drop them.",
							"100 never rewrites.", &sift_tbl_dead_rewrite_pct, SIFT_TBL_DEAD_PCT, 1, 100,
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
insert_xmin(SiftTamOptions options)
{
	return (options & TABLE_INSERT_FROZEN) ? FrozenTransactionId : GetCurrentTransactionId();
}

static void
insert_one(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, uint32 token)
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

/* A row of COPY, CREATE TABLE AS or REFRESH MATERIALIZED VIEW (they pass a bulk insert state) goes into a column group when sift.table_bulk_groups says so */
static bool
insert_bulk(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	SiftTblRel *st;
	MinimalTuple mtup;

	if (bistate == NULL || !sift_tbl_bulk_groups)
		return false;
	st = sift_tbl_rel(rel);
	sift_tbl_ensure_init(rel);
	sift_tbl_layout_ready(rel, st);
	if (!sift_tbl_load_wanted(rel, st, (options & TABLE_INSERT_FROZEN) != 0))
		return false;
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	mtup = form_mtuple(rel, slot);
	sift_tbl_load_row(rel, st, mtup, cid, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	heap_free_minimal_tuple(mtup);
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
		MinimalTuple mtup = form_mtuple(rel, slot);

		sift_tbl_log_change(rel, SIFT_TBL_OP_INSERT, mtup, NULL);
		heap_free_minimal_tuple(mtup);
	}
}

static void
tbl_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, SiftTamOptions options, BulkInsertState bistate)
{
	for (int i = 0; i < nslots; i++)
		if (!insert_bulk(rel, slots[i], cid, options, bistate))
			insert_one(rel, slots[i], cid, options, 0);
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
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not read the old row of \"%s\" for logical decoding", RelationGetRelationName(rel)),
				 errhint("A concurrent merge or defrag replaced the rows it reads: retry the statement.")));
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
			sift_tbl_tomb_append(rel, st, sift_key_from_tid(tid), GetCurrentTransactionId(), cid, SIFT_TBL_TOMB_LOCK, 0);
		UnlockTuple(rel, tid, ExclusiveLock);
		if (!sift_tbl_fetch_row(rel, st, tid, &rv))
			ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not read the locked tuple of \"%s\"", RelationGetRelationName(rel)),
				 errhint("A concurrent merge or defrag replaced the rows it reads: retry the statement.")));
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
