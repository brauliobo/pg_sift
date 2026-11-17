/*
 * core/cachebudget.c - the budget the per-backend caches share (core/cachebudget.h).
 */
#include "postgres.h"

#include <limits.h>

#include "utils/guc.h"

#include "core/cachebudget.h"

#define CACHE_KINDS		4

int			sift_backend_cache_kb = 256 * 1024;

static const SiftCacheKind *kinds[CACHE_KINDS];
static int	nkinds;
static uint64 clock_now;

void
sift_cache_budget_define_gucs(void)
{
	DefineCustomIntVariable("sift.backend_cache_kb",
							"Memory per backend that the caches of dictionaries, bm25 segments and joined-table rows keep together for the next statement (-1: only each cache's own setting limits it).",
							"The least recently used entry that no statement holds is dropped from whichever cache has it when another needs room; an entry that alone is larger than the total is not kept.",
							&sift_backend_cache_kb, 256 * 1024, -1, MAX_KILOBYTES,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
}

uint64
sift_cache_tick(void)
{
	return ++clock_now;
}

void
sift_cache_register(const SiftCacheKind *kind)
{
	Assert(nkinds < CACHE_KINDS);
	kinds[nkinds++] = kind;
}

static Size
held(void)
{
	Size		total = 0;

	for (int i = 0; i < nkinds; i++)
		total += kinds[i]->bytes();
	return total;
}

/* Drops the least recently used entries nobody pins, from any cache, until need more bytes fit the total; false when they do not */
bool
sift_cache_room(Size need)
{
	Size		limit;

	if (sift_backend_cache_kb < 0)
		return true;
	limit = (Size) sift_backend_cache_kb * 1024;
	if (need > limit)
		return false;
	while (held() + need > limit)
	{
		const SiftCacheKind *victim = NULL;
		uint64		best = UINT64_MAX;

		for (int i = 0; i < nkinds; i++)
		{
			uint64		t = kinds[i]->oldest();

			if (t < best)
			{
				best = t;
				victim = kinds[i];
			}
		}
		if (victim == NULL)
			return false;
		victim->evict();
	}
	return true;
}
