/*
 * agg/pct.c - the arena of the values of percentile_cont (agg/pct.c): a chunk added to an arena of half of MaxAllocSize grows it to MaxAllocSize, not past it, which an allocation refuses.
 */
#include "postgres.h"

#include "agg/pct.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

static SiftTextArena ar;
static V6Acc acc;

static void
add_one(void *arg)
{
	sift_v6_pct_add(&ar, &acc, 1.0);
}

static void
test_arena_past_max_alloc(void)
{
	ar = (SiftTextArena) {.cx = CurrentMemoryContext,.buf = palloc(4096),.cap = (uint64) 1 << 29,.used = ((uint64) 1 << 29) - 16};
	acc = (V6Acc) {0};
	SIFT_CHECK(pgshim_try(add_one, NULL));
	SIFT_CHECK(ar.cap == MaxAllocSize && ar.used <= ar.cap);
	pfree(ar.buf);
}

int
main(void)
{
	if (unit_wants("pct_arena_past_max_alloc"))
		test_arena_past_max_alloc();
	puts("pct: ok");
	return 0;
}
