/*
 * test/unit/table/f2/lock/touched.c - an allocation of the table of the rows a transaction holds that fails (out of memory) leaves the table as it was: every row it holds is
 * found, the next row grows it, and a rollback lets them all go (S118-04).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/oom.h"

#include "table/f2/lock/touched.c"

#undef MemoryContextAlloc
#undef MemoryContextAllocZero
#undef repalloc

static SubTransactionId cur_sub = TopSubTransactionId;
static uint32 next_row;

MemoryContext
sift_rowlock_txn_cx(void)
{
	return CurrentMemoryContext;
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return cur_sub;
}

TransactionId
GetCurrentTransactionId(void)
{
	return 100;
}

static SiftRowLockKey
key_of(uint32 row)
{
	return (SiftRowLockKey) {.locator = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384},.tid = (uint64) row << 16 | 1};
}

static void
add_next(void *arg pg_attribute_unused())
{
	SiftRowLockKey k = key_of(next_row);

	SIFT_CHECK(sift_touched_add(&k, 0));
	next_row++;
}

/* The rows up to n, the last one with its allocation number fail_at failing: it is not held, and the next try holds it */
static void
add_failing(uint32 n, int fail_at, const char *what)
{
	int			before;

	while (next_row + 1 < n)
		SIFT_CHECK(pgshim_try(add_next, NULL));
	oom_after = fail_at;
	SIFT_CHECKF(!pgshim_try(add_next, NULL) && pgshim_sqlstate == ERRCODE_OUT_OF_MEMORY, "%s: the allocation fails", what);
	oom_after = -1;
	SIFT_CHECKF(sift_touched_count() == next_row, "%s: %u rows held after the failure, %u before", what, sift_touched_count(), next_row);
	for (uint32 r = 0; r < next_row; r++)
	{
		SiftRowLockKey k = key_of(r);

		SIFT_CHECKF(is_touched(&k, sift_touched_at(r)->subxid), "%s: row %u is found after the failure", what, r);
	}
	before = oom_allocs;
	SIFT_CHECK(pgshim_try(add_next, NULL));
	SIFT_CHECKF(oom_allocs > before, "%s: the row after the failure grows the table again", what);
}

static void
release_sub(SubTransactionId sub)
{
	for (int32 i = sift_touched_sub_first(sub); i >= 0; i = sift_touched_sub_first(sub))
		(void) sift_touched_remove((uint32) i);
	sift_touched_sub_close(sub);
}

int
main(void)
{
	add_failing(TOUCHED_INDEXED_AT, 0, "the first index");
	add_failing(64 + 1, 0, "the rows");
	add_failing(96 + 1, 0, "a larger index");
	for (int s = 0; s < 7; s++)
	{
		cur_sub++;
		SIFT_CHECK(pgshim_try(add_next, NULL));
	}
	cur_sub++;
	add_failing(next_row + 1, 0, "the subtransactions");
	while (cur_sub > TopSubTransactionId)
		release_sub(cur_sub--);
	release_sub(TopSubTransactionId);
	SIFT_CHECKF(sift_touched_count() == 0, "the rollbacks let every row go: %u left", sift_touched_count());
	sift_touched_reset();
	return sift_check_summary();
}
