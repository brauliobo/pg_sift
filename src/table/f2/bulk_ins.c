/*
 * table/f2/bulk_ins.c - the direct route of a bulk insert into a sift_table (col/f2/batch.h).
 *
 * A multi_insert call of SIFT_BATCH_MIN_SLOTS rows or more, on a table that is in no publication and has no row trigger, packs each row into cells that the table keeps from one call to the
 * next and hands them to the stage of the transaction, which copies them into the column buffer of the partition: no allocation for a row. The values that need a copy of their own (a
 * detoasted datum) are made in a batch context that is reset once each pack of SIFT_BULK_PACK_ROWS rows is staged, and when the call ends by an error. The rows are packed and staged together. The stage writes the buffer of a
 * partition as one part of level 1 when it holds the flush count of rows (col/f2/l0/stage.c, sift_stage_set_bulk), so the rows are encoded and written once.
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "table/f2/tbl.h"
#include "core/f2/gate.h"
#include "core/memtrack.h"

#define SIFT_BULK_PACK_ROWS	64

typedef struct SiftTblBulkState
{
	SiftCell   *cells;			/* SIFT_BULK_PACK_ROWS rows of the cells of a row, and their keys and addresses */
	uint8	  (*keys)[SIFT_BOUND_BYTES];
	uint64	   *tids;
	SiftCell  **elem;			/* the elements of the lists of declared paths of each row of the batch (NULL: the table has none) */
	MemoryContext batch_cx;
	uint64		row_bytes;
} SiftTblBulkState;

static uint64
row_bytes_of(const SiftF2Store *store)
{
	int			nstr = 0;

	for (int c = 0; c < store->ncols; c++)
		nstr += store->kinds[c] == SIFT_CELL_STR;
	return sift_batch_row_bytes(store->ncols, nstr, 24);
}

static SiftTblBulkState *
bulk_state(SiftF2Store *store)
{
	SiftTblF2Extra *x = sift_tbl_extra(store);
	SiftTblBulkState *b = x->bulk;

	if (b != NULL)
		return b;
	b = MemoryContextAllocZero(store->cx, sizeof(SiftTblBulkState));
	b->cells = MemoryContextAlloc(store->cx, sizeof(SiftCell) * Max(store->ncols, 1) * SIFT_BULK_PACK_ROWS);
	b->keys = MemoryContextAlloc(store->cx, SIFT_BULK_PACK_ROWS * (Size) SIFT_BOUND_BYTES);
	b->tids = MemoryContextAlloc(store->cx, SIFT_BULK_PACK_ROWS * sizeof(uint64));
	b->elem = store->has_groups ? MemoryContextAlloc(store->cx, sizeof(SiftCell *) * Max(store->ncols, 1) * SIFT_BULK_PACK_ROWS) : NULL;
	b->batch_cx = sift_mem_allocset_create(store->cx, "sift cache bulk insert batch");
	b->row_bytes = row_bytes_of(store);
	x->bulk = b;
	return b;
}

SiftTxnStage *
sift_tbl_f2_stage_bulk(Relation rel, SiftF2Store *store)
{
	SiftTxnStage *st = sift_stage_get(rel, &store->spec);
	uint64		budget = sift_stage_bulk_budget();

	sift_stage_set_bulk(st, sift_batch_flush_rows(budget, row_bytes_of(store)), budget);
	return st;
}

/* Packs and stages n rows, then releases what their values needed */
static void
bulk_pack(Relation rel, SiftF2Store *store, SiftTblBulkState *b, SiftTxnStage *st, TupleTableSlot **slots, int n, TransactionId xid, CommandId cid)
{
	for (int i = 0; i < n; i++)
	{
		TupleTableSlot *slot = slots[i];
		ItemPointerData tid;

		slot_getallattrs(slot);
		sift_tbl_f2_tid_next(rel, store, &tid);
		sift_f2_store_pack_row(rel, store, b->batch_cx, &tid, slot->tts_values, slot->tts_isnull, true, b->cells + (size_t) i * store->ncols,
							   b->elem != NULL ? b->elem + (size_t) i * store->ncols : NULL, b->keys[i]);
		b->tids[i] = sift_tbl_tid_pack(&tid);
		slot->tts_tid = tid;
		slot->tts_tableOid = RelationGetRelid(rel);
	}
	sift_stage_add_batch(rel, st, n, b->keys, b->tids, xid, cid, b->cells, (const SiftCell *const *) b->elem);
	MemoryContextReset(b->batch_cx);
	SIFT_F2_GATE(SIFT_F2_GATE_BULK_INSERT);
	CHECK_FOR_INTERRUPTS();
}

void
sift_tbl_f2_bulk_insert(Relation rel, SiftF2Store *store, TupleTableSlot **slots, int nslots, CommandId cid, bool frozen)
{
	SiftTblBulkState *b = bulk_state(store);
	SiftTxnStage *st = sift_tbl_f2_stage_bulk(rel, store);
	TransactionId xid = frozen ? FrozenTransactionId : GetCurrentTransactionId();

	sift_tbl_f2_tid_ensure(rel, store, nslots);
	PG_TRY();
	{
		for (int from = 0; from < nslots; from += SIFT_BULK_PACK_ROWS)
			bulk_pack(rel, store, b, st, slots + from, Min(SIFT_BULK_PACK_ROWS, nslots - from), xid, frozen ? 0 : cid);
	}
	PG_FINALLY();
	{
		MemoryContextReset(b->batch_cx);
	}
	PG_END_TRY();
}
