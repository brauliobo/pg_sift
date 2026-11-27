/*
 * test_du64 - the sets of distinct integers (agg/distinct/u64.c) under a budget over 1 GB: a table that one allocation cannot hold is not grown, the set says it is full (the caller moves
 * its values to the set that spills), never asking for an allocation PostgreSQL refuses. A bucket of the grouped set that the budget lets grow no more is full when deduplicating it frees
 * less than a quarter of it: it does not take one pair after each deduplication, which would deduplicate it again for every pair that comes.
 */
#include "postgres.h"

#include <signal.h>

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "agg/distinct/u64.c"

#define BIG_BUDGET	((uint64) 4 << 30)

volatile sig_atomic_t InterruptPending = false;

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

/* the next value from *v whose pair of group 0 goes to bucket 0 */
static uint64
in_bucket0(uint64 *v)
{
	while (pair_hash(*v, 0) >> (64 - DU64G_BUCKET_BITS) != 0)
		(*v)++;
	return (*v)++;
}

/* a grouped set in buckets whose bucket 0 holds `distinct` values and `dups` repeats of its first one, at a budget that holds no more than it has */
static SiftDu64G *
bucket0_at_budget(int distinct, int dups, uint64 *first)
{
	SiftDu64G  *d = sift_du64g_create_ex(CurrentMemoryContext, BIG_BUDGET, DU64_FIRST_CAP);
	uint64		v = 0;

	*first = in_bucket0(&v);
	SIFT_CHECK(sift_du64g_add(d, 0, *first));
	for (int i = 1; i < distinct; i++)
		SIFT_CHECK(sift_du64g_add(d, 0, in_bucket0(&v)));
	buckets_dedup(d);
	for (int i = 0; i < dups; i++)
		SIFT_CHECK(sift_du64g_add(d, 0, *first));
	SIFT_CHECK(d->bk != NULL && d->bk[0].n == DU64G_BUCKET_FIRST && d->bk[0].cap == DU64G_BUCKET_FIRST);
	SIFT_CHECK(d->scratch_cap == 2 * DU64G_BUCKET_FIRST);
	d->budget = sift_du64g_bytes(d);
	return d;
}

/* the caller moves the set to the one that spills at its first false: a set that took the pair after one more deduplication would take each next one the same way */
static void
test_bucket_one_pair_short_of_full_is_full(void)
{
	uint64		first;
	SiftDu64G  *d = bucket0_at_budget(DU64G_BUCKET_FIRST - 1, 1, &first);
	int			taken = 0;

	while (taken < 1000 && sift_du64g_add(d, 0, first))
		taken++;
	SIFT_CHECK(taken == 0);
	SIFT_CHECK(d->bk[0].n == DU64G_BUCKET_FIRST - 1);
}

static void
test_bucket_a_quarter_of_repeats_takes_more(void)
{
	uint64		first;
	SiftDu64G  *d = bucket0_at_budget(DU64G_BUCKET_FIRST * 3 / 4, DU64G_BUCKET_FIRST / 4, &first);

	SIFT_CHECK(sift_du64g_add(d, 0, first));
	SIFT_CHECK(d->bk[0].n == DU64G_BUCKET_FIRST * 3 / 4 + 1);
}

int
main(void)
{
	test_flat_set_is_full_before_one_allocation_cannot_hold_it();
	test_grouped_set_is_full_before_one_allocation_cannot_hold_it();
	test_bucket_one_pair_short_of_full_is_full();
	test_bucket_a_quarter_of_repeats_takes_more();
	puts("test_du64: ok");
	return 0;
}
