/*
 * col/f2/l0/stage_test.c - the test-only SQL function of the deletes a stage holds: sift_test_f2_stage_spec_abort
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/f2/test.h"

PG_FUNCTION_INFO_V1(sift_test_f2_stage_spec_abort);

/*
 * (index, n, tid_base): the stage withdraws the speculative insertions of n rows one after the other, and writes what it holds. The result is how many withdrawals left the xmax of a
 * withdrawn insertion (FrozenTransactionId) where the element before the array of deletes would have it: what one that is written at the moment the array is full does when it names the
 * element it just added by the count the write emptied. The addresses are those of no part, so the write drops them.
 */
Datum
sift_test_f2_stage_spec_abort(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftPartSpec spec = f2t_spec(false);
	SiftTxnStage *st = sift_stage_get(rel, &spec);
	int			n = PG_GETARG_INT32(1);
	int64		tid_base = PG_GETARG_INT64(2);
	int			changed = 0;

	for (int i = 0; i < n; i++)
	{
		uint8		key[SIFT_BOUND_BYTES];
		const SiftStageDel *array = st->dels;
		TransactionId before = array != NULL ? array[-1].xmax : InvalidTransactionId;

		f2t_key(i, key);
		sift_stage_delete_spec_abort(st, key, f2t_tid(tid_base, i));
		changed += array != NULL && array == st->dels && before != FrozenTransactionId && array[-1].xmax == FrozenTransactionId;
	}
	sift_stage_flush(rel);
	index_close(rel, NoLock);
	PG_RETURN_INT32(changed);
}

#endif							/* SIFT_TESTING */
