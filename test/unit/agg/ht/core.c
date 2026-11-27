/*
 * agg/ht/core.c - the salted hash table (agg/ht/ht.c): a table whose slots or state blocks pass MaxAllocSize, below the work_mem of a large server, still grows.
 */
#include "postgres.h"

#include "agg/ht/ht.h"
#include "core/memtrack.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define SLOTS_PAST_1GB	((uint32) 15 << 20)
#define WIDE_STRIDE		((uint32) 1 << 20)

static SiftSaltedHT ht;

void
sift_mem_release_os(Size freed_bytes)
{
}

static void
reserve_slots(void *arg)
{
	sift_ht_reserve(&ht, SLOTS_PAST_1GB);
}

static void
presize_blocks(void *arg)
{
	sift_ht_presize(&ht, 1100);
}

static void
put_keys(uint32 n)
{
	for (uint64 i = 1; i <= n; i++)
	{
		const SiftInlineKey16 k = {.u64 = {i, 0}};
		bool		found;

		SIFT_CHECK(sift_ht_upsert(&ht, sift_hash_bytes(&i, sizeof(i)), &k, &found) != NULL && !found);
	}
	for (uint64 i = 1; i <= n; i++)
	{
		const SiftInlineKey16 k = {.u64 = {i, 0}};

		SIFT_CHECK(sift_ht_find(&ht, sift_hash_bytes(&i, sizeof(i)), &k) != NULL);
	}
}

static void
test_slots_past_max_alloc(void)
{
	sift_ht_init(&ht, 64);
	SIFT_CHECK(pgshim_try(reserve_slots, NULL));
	SIFT_CHECK((uint64) ht.capacity * sizeof(SiftAggSlot64) > MaxAllocSize && ht.grow_at >= SLOTS_PAST_1GB);
	put_keys(1000);
	sift_ht_destroy(&ht);
}

static void
test_blocks_past_max_alloc(void)
{
	sift_ht_init_ex(&ht, 64, false, WIDE_STRIDE, SIFT_HT_NO_EXT);
	SIFT_CHECK(pgshim_try(presize_blocks, NULL));
	SIFT_CHECK((uint64) ht.ext_cap * ht.ext_stride > MaxAllocSize);
	put_keys(1000);
	sift_ht_destroy(&ht);
}

int
main(void)
{
	if (unit_wants("ht_slots_past_max_alloc"))
		test_slots_past_max_alloc();
	if (unit_wants("ht_blocks_past_max_alloc"))
		test_blocks_past_max_alloc();
	puts("core: ok");
	return 0;
}
