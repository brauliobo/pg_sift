/*
 * agg/fifo.c - the queue of tuples of v6_fifo.h.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "agg/fifo.h"

void
sift_v6_fifo_open(SiftV6Fifo *f, MemoryContext query_cx)
{
	MemoryContext old = MemoryContextSwitchTo(query_cx);

	f->row_cx = AllocSetContextCreate(query_cx, "sift agg v6 tuple", ALLOCSET_SMALL_SIZES);
	f->store = tuplestore_begin_heap(false, false, Max(work_mem, SIFT_V6_FIFO_MIN_KB));
	MemoryContextSwitchTo(old);
}

void
sift_v6_fifo_put(SiftV6Fifo *f, TupleDesc desc, const Datum *values, const bool *isnull)
{
	tuplestore_putvalues(f->store, desc, (Datum *) values, (bool *) isnull);
	MemoryContextReset(f->row_cx);
}

TupleTableSlot *
sift_v6_fifo_get(SiftV6Fifo *f, TupleTableSlot *slot)
{
	if (tuplestore_gettupleslot(f->store, true, false, slot))
		return slot;
	tuplestore_clear(f->store);
	return NULL;
}

void
sift_v6_fifo_clear(SiftV6Fifo *f)
{
	tuplestore_clear(f->store);
}

void
sift_v6_fifo_close(SiftV6Fifo *f)
{
	if (f->store != NULL)
		tuplestore_end(f->store);
	if (f->row_cx != NULL)
		MemoryContextDelete(f->row_cx);
	*f = (SiftV6Fifo) {0};
}
