/*
 * col/test/alloc_test.c - exact memory accounting of a call: runs it n times in a context of its own and says how many bytes that holds.
 * A call that allocates for its caller and does not free (a leak in a long-lived context) shows as bytes that grow with n; a clean one holds the same bytes whatever n is.
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/genam.h"
#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/test/test_int.h"
#include "core/catname.h"

PG_FUNCTION_INFO_V1(sift_test_row_key_bytes);

/* sift._test_row_key_bytes(index regclass, n int) returns int8: the bytes that n questions whether the index tracks a key leave allocated in their context */
Datum
sift_test_row_key_bytes(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift test alloc", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	MemoryContextCounters c = {0};

	for (int i = PG_GETARG_INT32(1); i > 0; i--)
		(void) sift_col_track_on(index);
	MemoryContextSwitchTo(old);
	MemoryContextMemConsumed(cx, &c);
	MemoryContextDelete(cx);
	index_close(index, AccessShareLock);
	PG_RETURN_INT64((int64) (c.totalspace - c.freespace));
}
#endif
