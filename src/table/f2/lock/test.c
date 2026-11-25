/*
 * table/f2/lock/test.c - sift._test_rowlock_stats(): the entries of tier 1 of the row locks of a sift_table, in the table and in the chunks of the stripes that filled.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"

#include "table/f2/lock/esc.h"

PG_FUNCTION_INFO_V1(sift_test_rowlock_stats);

Datum
sift_test_rowlock_stats(PG_FUNCTION_ARGS)
{
	SiftRowLockShmLayout *layout = sift_lock_shm_attach();
	SiftRowLockCount total = {0};
	TupleDesc	desc;
	Datum		v[3];
	bool		n[3] = {false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	for (int si = 0; layout != NULL && si < SIFT_ROWLOCK_PARTITIONS; si++)
	{
		SiftRowLockCount c;

		LWLockAcquire(&layout->rows.stripe_locks[si].lock, LW_SHARED);
		c = sift_rowlock_count(&layout->rows, si);
		LWLockRelease(&layout->rows.stripe_locks[si].lock);
		total.in_table += c.in_table;
		total.in_chunks += c.in_chunks;
		total.chunks += c.chunks;
	}
	sift_rowlock_area_detach();
	v[0] = Int64GetDatum(total.in_table);
	v[1] = Int64GetDatum(total.in_chunks);
	v[2] = Int64GetDatum(total.chunks);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, n)));
}

#endif
