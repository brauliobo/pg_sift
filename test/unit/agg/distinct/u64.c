/*
 * test_du64 - the sets of distinct integers (agg/distinct/u64.c) under a budget over 1 GB: a table that one allocation cannot hold is not grown, the set says it is full (the caller moves
 * its values to the set that spills), never asking for an allocation PostgreSQL refuses.
 */
#include "postgres.h"

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "agg/distinct/u64.c"

#define BIG_BUDGET	((uint64) 4 << 30)

static bool grown;

static void
grow_flat(void *arg)
{
	SiftDistinctU64 *d = arg;

	d->cap = (uint64) 1 << 26;
	grown = grow(d);
}

static void
grow_pairs(void *arg)
{
	SiftDu64G  *d = arg;

	d->cap = (uint64) 1 << 26;
	grown = pairs_grow(d);
}

static void
test_flat_set_is_full_before_one_allocation_cannot_hold_it(void)
{
	SiftDistinctU64 *d = sift_du64_create(CurrentMemoryContext, BIG_BUDGET);

	grown = true;
	SIFT_CHECK(pgshim_try(grow_flat, d));
	SIFT_CHECK(!grown);
	pgshim_reset();
}

static void
test_grouped_set_is_full_before_one_allocation_cannot_hold_it(void)
{
	SiftDu64G  *d = sift_du64g_create(CurrentMemoryContext, BIG_BUDGET);

	grown = true;
	SIFT_CHECK(pgshim_try(grow_pairs, d));
	SIFT_CHECK(!grown);
	pgshim_reset();
}

int
main(void)
{
	test_flat_set_is_full_before_one_allocation_cannot_hold_it();
	test_grouped_set_is_full_before_one_allocation_cannot_hold_it();
	puts("test_du64: ok");
	return 0;
}
