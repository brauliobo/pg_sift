/*
 * agg/dictseen.c - a set made with a cap of work_mem >= 2 GB: the cap the set emits at leaves the arena and the table one allocation each, as a part adds its
 * bytes and its strings past the cap before the set emits them.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/dictseen.c"
#include "test/common/check.h"
#include "test/common/pgshim.h"

static void
test_cap_past_1gb_keeps_one_allocation(void)
{
	const uint64 caps[] = {(uint64) 2 << 30, (uint64) 8 << 30, UINT64_MAX};

	for (int i = 0; i < lengthof(caps); i++)
	{
		SiftDictSeen *d = sift_dictseen_create(NULL, caps[i]);
		const Size	arena = 2 * (d->cap + SIFT_DICTSEEN_MAX_BYTES);
		const Size	ht = (Size) pg_nextpower2_64((d->cap / sizeof(DictSlot) + SIFT_DICTSEEN_MAX_CODES) * 10 / 7) * sizeof(DictSlot);

		SIFT_CHECKF(arena <= MaxAllocSize && ht <= MaxAllocSize, "cap %llu: arena %zu, table %zu", (unsigned long long) caps[i], arena, ht);
		sift_dictseen_free(d);
	}
}

int
main(void)
{
	test_cap_past_1gb_keeps_one_allocation();
	return sift_check_summary();
}
