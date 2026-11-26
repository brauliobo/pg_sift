/*
 * test/unit/table/f2/col_delta_sub_abort.c - the books of the transaction stage after a rollback to a savepoint drops the column deltas of the subtransaction (table/f2/col_delta/flush.c,
 * SIFT_STAGE_SUB_ABORT): a stage left with no record has nothing pending for the next read of the relation, and the memory it tells the stages of the transaction is what it still holds.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

bool		sift_test_col_delta = true;
MemoryContext TopTransactionContext = (MemoryContext) 1;

#include "table/f2/col_delta/col_delta.c"
#include "table/f2/col_delta/batch_enc.c"
#include "table/f2/col_delta/flush.c"

void
sift_stage_host_staged(SiftTxnStage *st, uint64 bytes)
{
	st->host_mem = bytes;
}

/* A record of row `row` staged by subtransaction `sub`, with the books the host keeps (sift_col_delta_stage_plan) */
static void
stage(SiftTxnStage *st, uint64 row, SubTransactionId sub)
{
	SiftColDeltaTarget t = {.part_gen = 7,.granule_id = 0,.row_ordinal = (uint32) row,.tid = (row << 16) | 1};
	SiftColDeltaCol col = {.attnum = 2,.kind = SIFT_CELL_INT,.cell.i = (int64) row};
	uint8		rec[SIFT_COL_DELTA_MAX_REC_BYTES];
	uint8		key_lo[SIFT_BOUND_BYTES] = {0};

	SIFT_CHECKF(sift_col_delta_encode(rec, sizeof(rec), &t, 100, 0, 0, &col, 1) > 0, "row %llu: the record is encoded", (unsigned long long) row);
	sift_col_delta_stage_add(st->host_stage, 1, key_lo, sub, rec);
	st->host_pending++;
	st->host_mem = ((const SiftColDeltaStage *) st->host_stage)->bytes;
}

static void
check_abort(int top_rows, int sub_rows, const char *what)
{
	SiftTxnStage st = {.host_stage = MemoryContextAllocZero(TopTransactionContext, sizeof(SiftColDeltaStage))};
	uint64		top_bytes;

	for (int i = 0; i < top_rows; i++)
		stage(&st, (uint64) i + 1, TopSubTransactionId);
	top_bytes = ((const SiftColDeltaStage *) st.host_stage)->bytes;
	for (int i = 0; i < sub_rows; i++)
		stage(&st, (uint64) (top_rows + i) + 1, TopSubTransactionId + 1);
	sift_col_delta_stage_event(NULL, &st, SIFT_STAGE_SUB_ABORT, TopSubTransactionId + 1, TopSubTransactionId);
	SIFT_CHECKF((st.host_pending > 0) == (top_rows > 0), "%s: %u records pending, the stage holds %d", what, st.host_pending, top_rows);
	SIFT_CHECKF(st.host_mem == top_bytes, "%s: the stage tells %llu bytes, it holds %llu", what, (unsigned long long) st.host_mem, (unsigned long long) top_bytes);
}

int
main(void)
{
	check_abort(0, 1, "a savepoint that staged one record");
	check_abort(0, 40, "a savepoint that staged 40 records");
	check_abort(3, 5, "three records of the transaction and five of the savepoint");
	check_abort(4, 0, "a savepoint that staged nothing");
	return sift_check_summary();
}
