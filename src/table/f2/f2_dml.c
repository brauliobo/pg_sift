/*
 * table/f2/f2_dml.c - INSERT, COPY, ON CONFLICT, DELETE and UPDATE of a sift_table.
 *
 * Nothing refuses a statement: a row is cells and a key, staged in memory by the transaction (col/f2/l0_stage.h) and written as parts when the stage is full, when the transaction
 * is about to commit or when the backend reads the table. A statement that aborts leaves nothing on disk. The address of a row is made here, from a block of virtual blocks reserved
 * in the metapage; an UPDATE is the delete of the old version and the insert of a new one at a new address, except the one that changes a few columns of a wide row of a published part:
 * that is a column delta (table/f2/f2_col_delta.h) and the row keeps its address.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "pgstat.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/reltrigger.h"
#include "utils/snapmgr.h"

#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_gcache.h"
#include "table/f2/f2_tbl.h"
#include "table/f2/f2_vm.h"

/* ---- addresses ---- */

StaticAssertDecl(SIFT_BATCH_VBLOCK_ROWS == SIFT_TBL_ROWS_PER_VBLOCK, "the batch policy counts the rows of a virtual block as the table does");

static void
tid_refill(Relation rel, SiftTblTidPool *p, uint64 vblocks)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	p->next_vblock = m->next_vblock;
	m->next_vblock += vblocks;
	p->end_vblock = m->next_vblock;
	p->next_off = FirstOffsetNumber;
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
	sift_tbl_vm_lease_set(rel, p->next_vblock, p->end_vblock);
}

void
sift_tbl_f2_tid_next(Relation rel, SiftF2Store *store, ItemPointer tid)
{
	SiftTblTidPool *p = &sift_tbl_extra(store)->pool;

	if (p->next_vblock >= p->end_vblock)
	{
		/* STOR[STOR-TBL-11]: adaptive vblock TID reserve (16->1024 vblocks; tightens SEC_TID_INDEX delta width) */
		p->refill_vblocks = Min(Max(p->refill_vblocks * 4, SIFT_TBL_VBLOCK_FIRST_RESERVE), SIFT_TBL_VBLOCK_RESERVE);
		sift_tbl_vm_lease_open(rel);
		tid_refill(rel, p, p->refill_vblocks);
	}
	ItemPointerSet(tid, (BlockNumber) p->next_vblock, p->next_off);
	if (++p->next_off > SIFT_TBL_ROWS_PER_VBLOCK)
	{
		p->next_vblock++;
		p->next_off = FirstOffsetNumber;
		sift_tbl_vm_lease_roll(rel, p->next_vblock);
	}
}

void
sift_tbl_f2_tid_ensure(Relation rel, SiftF2Store *store, uint64 nrows)
{
	SiftTblTidPool *p = &sift_tbl_extra(store)->pool;
	uint64		left = p->end_vblock > p->next_vblock ? (p->end_vblock - p->next_vblock) * SIFT_TBL_ROWS_PER_VBLOCK - (p->next_off - FirstOffsetNumber) : 0;

	if (left < nrows)
	{
		sift_tbl_vm_lease_open(rel);
		tid_refill(rel, p, Max(sift_batch_vblocks(nrows, SIFT_TBL_VBLOCK_RESERVE), SIFT_TBL_VBLOCK_BULK_RESERVE));
	}
}

void
sift_tbl_f2_tid_reset(SiftF2Store *store)
{
	sift_tbl_extra(store)->pool = (SiftTblTidPool) {0};
}

/* ---- logical replication ---- */

void
sift_tbl_f2_log_guard(Relation rel, uint64 newlen, uint64 oldlen)
{
	uint64		total = newlen + oldlen + 64;

	if (newlen > SIFT_TBL_LOG_MAX_BYTES || oldlen > SIFT_TBL_LOG_MAX_BYTES || total > SIFT_TBL_LOG_MAX_BYTES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("logical replication WAL record for sift_table \"%s\" is %llu bytes, exceeding the %llu-byte limit", RelationGetRelationName(rel),
							   (unsigned long long) total, (unsigned long long) SIFT_TBL_LOG_MAX_BYTES),
						errdetail("sift_table stores wide rows without TOAST; a logical replication change record must fit within %llu bytes (new=%llu, old=%llu).",
								  (unsigned long long) SIFT_TBL_LOG_MAX_BYTES, (unsigned long long) newlen, (unsigned long long) oldlen),
						errhint("Use a primary key REPLICA IDENTITY instead of FULL, keep values above 256 MB out of published sift_table columns, or exclude the table from logical publications.")));
}

static HeapTuple
identity_of_old(Relation rel, TupleTableSlot *old)
{
	HeapTuple	full,
				key;

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_NOTHING)
		return NULL;
	slot_getallattrs(old);
	full = heap_form_tuple(RelationGetDescr(rel), old->tts_values, old->tts_isnull);
	key = sift_tbl_old_key(rel, full);
	if (key != full)
		heap_freetuple(full);
	return key;
}

static void
log_published(Relation rel, SiftTblOp op, TupleTableSlot *newslot, TupleTableSlot *oldslot)
{
	bool		free_new = false;
	MinimalTuple newrow = NULL;
	HeapTuple	old = oldslot != NULL ? identity_of_old(rel, oldslot) : NULL;

	if (newslot != NULL)
		newrow = ExecFetchSlotMinimalTuple(newslot, &free_new);
	sift_tbl_f2_log_guard(rel, newrow != NULL ? newrow->t_len : 0, old != NULL ? old->t_len : 0);
	sift_tbl_log_key(rel, op, newrow, old);
	if (free_new)
		pfree(newrow);
	if (old != NULL)
		heap_freetuple(old);
}

static void
log_change(Relation rel, SiftTblOp op, TupleTableSlot *newslot, TupleTableSlot *oldslot)
{
	if (sift_tbl_check_publication(rel))
		log_published(rel, op, newslot, oldslot);
}

/* ---- staging a row ---- */

static uint64
stage_row(Relation rel, SiftF2Store *store, TupleTableSlot *slot, CommandId cid, bool frozen)
{
	SiftCell   *cells = palloc(sizeof(SiftCell) * Max(store->ncols, 1));
	uint8		key[SIFT_BOUND_BYTES];
	ItemPointerData tid;
	uint64		packed;

	slot_getallattrs(slot);
	sift_tbl_f2_tid_next(rel, store, &tid);
	packed = sift_tbl_tid_pack(&tid);
	sift_f2_store_pack_row(rel, store, store->row_cx, &tid, slot->tts_values, slot->tts_isnull, true, cells, NULL, key);
	sift_stage_add(rel, sift_stage_get(rel, &store->spec), key, packed, frozen ? FrozenTransactionId : GetCurrentTransactionId(), frozen ? 0 : cid, cells, NULL);
	MemoryContextReset(store->row_cx);
	pfree(cells);
	slot->tts_tid = tid;
	slot->tts_tableOid = RelationGetRelid(rel);
	return packed;
}

static void
key_of_slot(Relation rel, SiftF2Store *store, TupleTableSlot *slot, uint64 tid, uint8 *key)
{
	SiftCell   *cells = palloc(sizeof(SiftCell) * Max(store->ncols, 1));
	ItemPointerData ip;

	slot_getallattrs(slot);
	sift_tbl_tid_unpack(&ip, tid);
	sift_f2_store_pack_row(rel, store, store->row_cx, &ip, slot->tts_values, slot->tts_isnull, false, cells, NULL, key);
	MemoryContextReset(store->row_cx);
	pfree(cells);
}

void
sift_tbl_f2_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options pg_attribute_unused(), struct BulkInsertStateData *bistate pg_attribute_unused())
{
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	stage_row(rel, sift_f2_store_open(rel), slot, cid, false);
	log_change(rel, SIFT_TBL_OP_INSERT, slot, NULL);
	pgstat_count_heap_insert(rel, 1);
}

static bool
has_row_triggers(Relation rel)
{
	const TriggerDesc *t = rel->trigdesc;

	return t != NULL && (t->trig_insert_before_row || t->trig_insert_after_row || t->trig_insert_instead_row);
}

void
sift_tbl_f2_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, SiftTamOptions options pg_attribute_unused(), struct BulkInsertStateData *bistate pg_attribute_unused())
{
	SiftF2Store *store = sift_f2_store_open(rel);
	bool		published = sift_tbl_check_publication(rel);

	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	if (sift_batch_route(nslots, published, has_row_triggers(rel)) == SIFT_BATCH_DIRECT)
	{
		sift_tbl_f2_bulk_insert(rel, store, slots, nslots, cid);
	}
	else
		for (int i = 0; i < nslots; i++)
		{
			stage_row(rel, store, slots[i], cid, false);
			if (published)
				log_published(rel, SIFT_TBL_OP_INSERT, slots[i], NULL);
			CHECK_FOR_INTERRUPTS();
		}
	pgstat_count_heap_insert(rel, nslots);
}

void
sift_tbl_f2_finish_bulk_insert(Relation rel, SiftTamOptions options pg_attribute_unused())
{
	sift_stage_flush(rel);
	sift_stage_discard(RelationGetRelid(rel));
}

void
sift_tbl_f2_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, SiftTamOptions options pg_attribute_unused(), struct BulkInsertStateData *bistate pg_attribute_unused(),
									 uint32 specToken)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	uint64		tid = stage_row(rel, store, slot, cid, false);

	sift_tbl_rowlock_set_speculative(rel, tid, specToken, true);
	if (sift_tbl_extra(store)->has_partial_or_expr_unique)
		sift_stage_flush(rel);
	pgstat_count_heap_insert(rel, 1);
}

void
sift_tbl_f2_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken pg_attribute_unused(), bool succeeded)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	uint64		tid = sift_tbl_tid_pack(&slot->tts_tid);
	uint8		key[SIFT_BOUND_BYTES];

	bool		need_flush = sift_tbl_rowlock_spec_done(rel, tid, succeeded);

	if (!succeeded)
	{
		SiftTxnStage *st = sift_stage_get(rel, &store->spec);

		if (!sift_stage_cancel_insert(st, tid))
		{
			key_of_slot(rel, store, slot, tid, key);
			sift_stage_delete_spec_abort(st, key, tid);
		}
		pgstat_count_heap_delete(rel);
		return;
	}
	if (need_flush)
	{
		sift_stage_flush(rel);
		sift_tbl_rowlock_staged_flushed(rel, tid);
	}
	log_change(rel, SIFT_TBL_OP_INSERT, slot, NULL);
}

/* ---- the row as it is ---- */

/* The row as it is now, or NULL when it left the table: a TTL drop of its part (col/f2/f2_ttl.h) between the scan that found it and this statement is a delete by another statement */
static TupleTableSlot *
fetch_current(Relation rel, ItemPointer tid)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);

	if (sift_tbl_f2_tuple_fetch_row_version(rel, tid, SnapshotAny, slot))
		return slot;
	ExecDropSingleTupleTableSlot(slot);
	return NULL;
}

static bool
crosscheck_fails(Relation rel, ItemPointer tid, Snapshot crosscheck)
{
	TupleTableSlot *slot;
	bool		seen;

	if (!IsMVCCSnapshot(crosscheck))
		return false;
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	seen = sift_tbl_f2_tuple_fetch_row_version(rel, tid, crosscheck, slot);
	ExecDropSingleTupleTableSlot(slot);
	return !seen;
}

TM_Result
sift_tbl_f2_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	uint64		packed = sift_tbl_tid_pack(tid);
	bool		waited;
	TM_Result	res = sift_tbl_f2_lock_for_modify(rel, packed, LockTupleExclusive, wait, snapshot, cid, tmfd, &waited);
	TupleTableSlot *old;
	uint8		key[SIFT_BOUND_BYTES];

	if (res != TM_Ok)
		return res;
	if (crosscheck != InvalidSnapshot && crosscheck_fails(rel, tid, crosscheck))
	{
		tmfd->ctid = *tid;
		return TM_Updated;
	}
	sift_tbl_f2_conflict_in(rel, packed);
	old = fetch_current(rel, tid);
	if (old == NULL)
		return TM_Deleted;
	sift_tbl_vm_clear(rel, ItemPointerGetBlockNumber(tid));
	key_of_slot(rel, store, old, packed, key);
	sift_stage_delete(sift_stage_get(rel, &store->spec), key, packed);
	sift_tbl_f2_record_delete(rel, packed, cid, changingPart);
	log_change(rel, SIFT_TBL_OP_DELETE, NULL, old);
	ExecDropSingleTupleTableSlot(old);
	pgstat_count_heap_delete(rel);
	return TM_Ok;
}

bool
sift_tbl_f2_attr_equal(Form_pg_attribute att, Datum a, bool anull, Datum b, bool bnull)
{
	if (anull || bnull)
		return anull == bnull;
	if (att->attlen == -1)
		return datumIsEqual(PointerGetDatum(PG_DETOAST_DATUM(a)), PointerGetDatum(PG_DETOAST_DATUM(b)), false, -1);
	return datumIsEqual(a, b, att->attbyval, att->attlen);
}

/* The patch an UPDATE of a row of a published part would be, or why it is none; false when the row left the table */
static bool
plan_col_delta(Relation rel, SiftF2Store *store, uint64 tid, TupleTableSlot *old, TupleTableSlot *new, SiftColDeltaPlan *plan)
{
	SiftF2Loc	loc;

	if (!sift_f2_locate_cached(rel, store, tid, &loc))
		return false;
	sift_col_delta_plan(rel, store, &loc, old, new, plan);
	sift_f2_gbuf_unref(loc.gb);
	return true;
}

/* The columns of the unique and primary keys are as they were: the update takes a lock that a foreign key check on the row does not conflict with */
static bool
key_intact(Relation rel, SiftF2Store *store, TupleTableSlot *old, TupleTableSlot *new)
{
	int			a = -1;

	slot_getallattrs(old);
	slot_getallattrs(new);
	while ((a = bms_next_member(sift_tbl_extra(store)->key_attrs, a)) >= 0)
	{
		int			attno = a + FirstLowInvalidHeapAttributeNumber;

		if (attno > 0 && !sift_tbl_f2_attr_equal(TupleDescAttr(RelationGetDescr(rel), attno - 1), old->tts_values[attno - 1], old->tts_isnull[attno - 1], new->tts_values[attno - 1], new->tts_isnull[attno - 1]))
			return false;
	}
	return true;
}

TM_Result
sift_tbl_f2_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
						 LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	uint64		old_packed = sift_tbl_tid_pack(otid),
				new_packed;
	TupleTableSlot *old;
	TM_Result	res;
	bool		waited;
	uint8		old_key[SIFT_BOUND_BYTES];
	SiftColDeltaPlan patch;

	old = fetch_current(rel, otid);
	if (old == NULL)
		return TM_Deleted;
	*lockmode = key_intact(rel, store, old, slot) ? LockTupleNoKeyExclusive : LockTupleExclusive;
	res = sift_tbl_f2_lock_for_modify(rel, old_packed, *lockmode, wait, snapshot, cid, tmfd, &waited);
	if (res == TM_Ok && crosscheck != InvalidSnapshot && crosscheck_fails(rel, otid, crosscheck))
	{
		tmfd->ctid = *otid;
		res = TM_Updated;
	}
	if (res != TM_Ok)
	{
		ExecDropSingleTupleTableSlot(old);
		return res;
	}
	if (waited)
	{
		ExecDropSingleTupleTableSlot(old);
		old = fetch_current(rel, otid);
	}
	if (old == NULL || !plan_col_delta(rel, store, old_packed, old, slot, &patch))
	{
		if (old != NULL)
			ExecDropSingleTupleTableSlot(old);
		return TM_Deleted;
	}
	sift_tbl_f2_conflict_in(rel, old_packed);
	sift_tbl_vm_clear(rel, ItemPointerGetBlockNumber(otid));
	if (patch.reason == SIFT_CD_OK)
	{
		sift_col_delta_stage_plan(rel, &patch, cid);
		sift_tbl_rowlock_mark_col_delta(rel, old_packed, cid);
		slot->tts_tid = *otid;
		slot->tts_tableOid = RelationGetRelid(rel);
		*update_indexes = patch.summarizing ? TU_Summarizing : TU_None;
	}
	else
	{
		key_of_slot(rel, store, old, old_packed, old_key);
		new_packed = stage_row(rel, store, slot, cid, false);
		sift_stage_delete_update(sift_stage_get(rel, &store->spec), old_key, old_packed, new_packed);
		sift_tbl_f2_record_update(rel, old_packed, new_packed, cid);
		if (sift_tbl_rowlock_is_escalated(rel) && sift_stage_peek(rel)->bulk_rows == 0)
			sift_tbl_f2_stage_bulk(rel, store);
		*update_indexes = TU_All;
	}
	log_change(rel, SIFT_TBL_OP_UPDATE, slot, old);
	ExecDropSingleTupleTableSlot(old);
	pgstat_count_heap_update(rel, patch.reason == SIFT_CD_OK, false);
	return TM_Ok;
}

/* ---- a rewrite ---- */

void
sift_tbl_f2_bulk_begin(SiftTblF2Bulk *b, Relation rel, CommandId cid, bool frozen)
{
	b->rel = rel;
	b->store = sift_f2_store_open(rel);
	b->stage = sift_tbl_f2_stage_bulk(rel, b->store);
	b->frozen = frozen;
	b->cid = cid;
	b->nrows = 0;
}

void
sift_tbl_f2_bulk_add(SiftTblF2Bulk *b, TupleTableSlot *slot, ItemPointer tid)
{
	stage_row(b->rel, b->store, slot, b->cid, b->frozen);
	*tid = slot->tts_tid;
	b->nrows++;
	CHECK_FOR_INTERRUPTS();
}

void
sift_tbl_f2_bulk_end(SiftTblF2Bulk *b)
{
	sift_stage_flush(b->rel);
	sift_stage_discard(RelationGetRelid(b->rel));
}
