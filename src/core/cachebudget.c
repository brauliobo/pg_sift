/*
 * core/cachebudget.c - the budget the per-backend caches share (core/cachebudget.h).
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "core/cachebudget.h"
#include "core/host.h"
#include "core/memtrack.h"

#define CACHE_BLOCK_MAX		(64 * 1024)
#define CACHE_TRIM_BYTES	((Size) 256 * 1024)

int			sift_backend_cache_kb = 16 * 1024;

#ifdef SIFT_TESTING
bool		sift_cache_stress;
#endif

static MemoryContext cache_root;
static const SiftCacheKind *kinds[SIFT_CACHE_MAX_KINDS];
static int	nkinds;
static uint64 clock_now;
static Size reclaimed;
static uint64 xact_floor;

MemoryContext
sift_cache_root_context(void)
{
	if (cache_root == NULL)
		cache_root = AllocSetContextCreate(TopMemoryContext, "SiftBackendCacheContext", 8 * 1024, 8 * 1024, CACHE_BLOCK_MAX);
	return cache_root;
}

MemoryContext
sift_cache_create_context_in(MemoryContext parent, const char *name)
{
	return AllocSetContextCreateInternal(parent, name, ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, CACHE_BLOCK_MAX);
}

MemoryContext
sift_cache_create_context(const char *name)
{
	return sift_cache_create_context_in(sift_cache_root_context(), name);
}

void
sift_cache_adopt(MemoryContext cx)
{
	MemoryContextSetParent(cx, sift_cache_root_context());
}

Size
sift_cache_total_bytes(void)
{
	return cache_root ? MemoryContextMemAllocated(cache_root, true) : 0;
}

uint64
sift_cache_tick(void)
{
	return ++clock_now;
}

bool
sift_cache_used_in_xact(uint64 tick)
{
	return tick > xact_floor;
}

void
sift_cache_register(const SiftCacheKind *kind)
{
	SIFT_INVARIANT(nkinds < SIFT_CACHE_MAX_KINDS, "sift: more caches than the %d of core/cachebudget.h", SIFT_CACHE_MAX_KINDS);
	kinds[nkinds++] = kind;
}

static Size
freed_since(Size before)
{
	return before - Min(before, sift_cache_total_bytes());
}

static bool
evict_oldest(void)
{
	const SiftCacheKind *victim = NULL;
	uint64		best = UINT64_MAX;
	Size		before = sift_cache_total_bytes();
	bool		evicted;

	for (int i = 0; i < nkinds; i++)
	{
		uint64		t = kinds[i]->oldest();

		if (t < best)
		{
			best = t;
			victim = kinds[i];
		}
	}
	evicted = victim != NULL && victim->evict_one();
	reclaimed += freed_since(before);
	return evicted;
}

static void
trim_reclaimed(void)
{
	if (reclaimed < CACHE_TRIM_BYTES)
		return;
	sift_mem_trim_os();
	reclaimed = 0;
}

bool
sift_cache_room(Size need)
{
	Size		limit = (Size) sift_backend_cache_kb * 1024;
	bool		fits = need <= limit;

#ifdef SIFT_TESTING
	while (sift_cache_stress && evict_oldest())
		;
#endif
	while (fits && sift_cache_total_bytes() + need > limit)
		fits = evict_oldest();
	trim_reclaimed();
	return fits;
}

bool
sift_cache_commit(void)
{
	return sift_cache_room(0);
}

static void
cache_xact_event(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
		return;
	xact_floor = clock_now;
	if (cache_root != NULL)
		sift_cache_commit();
}

static void
backend_cache_assign(int newval, void *extra)
{
	Size		before;

	if (cache_root == NULL)
		return;
	before = sift_cache_total_bytes();
	if (newval == 0)
	{
		for (int i = 0; i < nkinds; i++)
			kinds[i]->evict_all();
		reclaimed += freed_since(before);
	}
	else
		while (sift_cache_total_bytes() > (Size) newval * 1024 && evict_oldest())
			;
	trim_reclaimed();
}

void
sift_cache_budget_define_gucs(void)
{
	RegisterXactCallback(cache_xact_event, NULL);
	DefineCustomIntVariable("sift.backend_cache_kb",
							"Memory per backend that every cache of the extension keeps together for the next statement (0: no cache is kept).",
							"The least recently used entry that no statement holds is dropped from whichever cache has it when another needs room; an entry that alone is larger than the total is not kept.",
							&sift_backend_cache_kb, 16 * 1024, 0, MAX_KILOBYTES,
							PGC_SUSET, GUC_UNIT_KB, NULL, backend_cache_assign, NULL);
}
