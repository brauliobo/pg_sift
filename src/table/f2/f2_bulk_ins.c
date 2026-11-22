/*
 * table/f2/f2_bulk_ins.c - the direct route of a bulk insert into a sift_table (col/f2/f2_batch.h).
 *
 * A multi_insert call of SIFT_BATCH_MIN_SLOTS rows or more, on a table that is in no publication and has no row trigger, packs each row into cells that the table keeps from one call to the
 * next and hands them to the stage of the transaction, which copies them into the column buffer of the partition: no allocation for a row. The values that need a copy of their own (a
 * detoasted datum) are made in a batch context that is reset every SIFT_BATCH_RESET_ROWS rows. The stage writes the buffer of a partition as one part of the cold tier when it holds the flush
 * count of rows (col/f2/l0_stage.c, sift_stage_set_bulk), so the rows are compressed once, with the dictionaries of the whole part, and written once.
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "table/f2/f2_tbl.h"
#include "core/memtrack.h"

typedef struct SiftTblBulkState
{
	SiftCell   *cells;
	MemoryContext batch_cx;
	uint32		since_reset;
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
	b->cells = MemoryContextAlloc(store->cx, sizeof(SiftCell) * Max(store->ncols, 1));
	b->batch_cx = sift_mem_allocset_create(store->cx, "sift bulk insert batch");
	b->row_bytes = row_bytes_of(store);
	x->bulk = b;
	return b;
}

SiftTxnStage *
sift_tbl_f2_stage_bulk(Relation rel, SiftF2Store *store)
{
	SiftTxnStage *st = sift_stage_get(rel, &store->spec);
	uint64		budget = (uint64) Max(work_mem, maintenance_work_mem / 4) * 1024;

	sift_stage_set_bulk(st, sift_batch_flush_rows(budget, row_bytes_of(store)), budget);
	return st;
}

void
sift_tbl_f2_bulk_insert(Relation rel, SiftF2Store *store, TupleTableSlot **slots, int nslots, CommandId cid)
{
	SiftTblBulkState *b = bulk_state(store);
	SiftTxnStage *st = sift_tbl_f2_stage_bulk(rel, store);
	TransactionId xid = GetCurrentTransactionId();

	sift_tbl_f2_tid_ensure(rel, store, nslots);
	for (int i = 0; i < nslots; i++)
	{
		TupleTableSlot *slot = slots[i];
		ItemPointerData tid;
		uint8		key[SIFT_BOUND_BYTES];

		slot_getallattrs(slot);
		sift_tbl_f2_tid_next(rel, store, &tid);
		sift_f2_store_pack_row(rel, store, b->batch_cx, &tid, slot->tts_values, slot->tts_isnull, true, b->cells, NULL, key);
		sift_stage_add(rel, st, key, sift_tbl_tid_pack(&tid), xid, cid, b->cells, NULL);
		slot->tts_tid = tid;
		slot->tts_tableOid = RelationGetRelid(rel);
		if (sift_batch_reset_due(&b->since_reset, 1))
			MemoryContextReset(b->batch_cx);
		CHECK_FOR_INTERRUPTS();
	}
}
