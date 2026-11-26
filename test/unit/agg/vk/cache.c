/*
 * test_vk_cache - the cache of decoded chunks (agg/vk/cache.c) with no device: a table full at its largest size evicts slabs that hold entries until
 * it has room, and a slab no chunk ever went to is never chosen, so an insert always ends.
 */
#include "postgres.h"

#include <unistd.h>

#include "agg/vk/job.h"
#include "test/common/check.h"

enum { NSLAB = 4, NENT = 8192 };

static SiftVk *
small_cache(void)
{
	SiftVk	   *vk = calloc(1, sizeof(*vk));

	vk->nslab = NSLAB;
	vk->slab = calloc(NSLAB, sizeof(Slab));
	for (uint32_t i = 0; i < NSLAB; i++)
		vk->slab[i].ent_head = VK_ENT_NONE;
	vk->nent = vk->nent_max = NENT;
	vk->ent = calloc(NENT, sizeof(Ent));
	vk->cur = -1;
	vk->clock = 1;
	return vk;
}

static uint32_t
slab_entries(const SiftVk *vk)
{
	uint32_t	n = 0;

	for (uint32_t i = 0; i < vk->nslab; i++)
		n += vk->slab[i].ent_count;
	return n;
}

static void
test_full_table_evicts_a_slab_with_entries(void)
{
	SiftVk	   *vk = small_cache();
	const uint32_t n = NENT / 4 * 3 + 1;
	uint64_t	gen0;
	uint32_t	woff;

	alarm(10);
	for (uint32_t i = 0; i + 1 < n; i++)
		SIFT_CHECK(cache_insert(vk, &(SiftVkKey) {.granule_id = i + 1}, 2, &woff) != NULL);
	gen0 = vk->evict_gen;
	SIFT_CHECK(cache_insert(vk, &(SiftVkKey) {.granule_id = n}, 2, &woff) != NULL);
	alarm(0);
	SIFT_CHECK(vk->evict_gen > gen0 && vk->evict_gen - gen0 <= NSLAB);
	SIFT_CHECK((uint64_t) vk->used * 4 < (uint64_t) vk->nent * 3);
	SIFT_CHECK(vk->used == slab_entries(vk));
	SIFT_CHECK(cache_find(vk, &(SiftVkKey) {.granule_id = n}) != NULL);
	free(vk->ent);
	free(vk->slab);
	free(vk);
}

static void
test_every_slab_pinned_gives_no_entry(void)
{
	SiftVk	   *vk = small_cache();
	const uint32_t n = NENT / 4 * 3;
	uint32_t	woff;

	vk->batch = 1;
	alarm(10);
	for (uint32_t i = 0; i < n; i++)
		SIFT_CHECK(cache_insert(vk, &(SiftVkKey) {.granule_id = i + 1}, 2, &woff) != NULL);
	SIFT_CHECK(cache_insert(vk, &(SiftVkKey) {.granule_id = n + 1}, 2, &woff) == NULL);
	alarm(0);
	SIFT_CHECK(vk->used == n);
	free(vk->ent);
	free(vk->slab);
	free(vk);
}

int
main(void)
{
	test_full_table_evicts_a_slab_with_entries();
	test_every_slab_pinned_gives_no_entry();
	puts("test_vk_cache: ok");
	return 0;
}
