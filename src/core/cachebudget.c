/*
 * core/cachebudget.c - the budget the per-backend caches share (core/cachebudget.h).
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "col/compress.h"
#include "core/cachebudget.h"
#include "core/dispatch.h"
#include "core/host.h"
#include "core/memtrack.h"

#define CACHE_BLOCK_MAX		(64 * 1024)
#define CACHE_TRIM_BYTES	((Size) 256 * 1024)
#define CACHE_IDLE_TRIM_FREE_BYTES	((Size) 128 * 1024)

int			sift_backend_cache_kb = 32 * 1024;

#ifdef SIFT_TESTING
bool		sift_cache_stress;
#endif

static MemoryContext cache_root;
static const SiftCacheKind *kinds[SIFT_CACHE_MAX_KINDS];
static int	nkinds;
static uint64 clock_now;
static Size reclaimed;
static uint64 xact_floor;
static MemoryContext charged;
static MemoryContextCallback charged_cb;

MemoryContext
sift_cache_root_context(void)
{
	if (cache_root == NULL)
		/* MEM[MEM-IDLE-08]: TopMemoryContext SiftBackendCacheContext root (1 KB min; capped by backend_cache_kb) */
		cache_root = AllocSetContextCreate(TopMemoryContext, "SiftBackendCacheContext", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, CACHE_BLOCK_MAX);
	return cache_root;
}

/* MEM[MEM-CORE-12]: cachebudget child ctx factory (1 KB init / 256 KB max; small sizes) */
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

static void
uncharge(void *arg)
{
	charged = NULL;
}

void
sift_cache_charge(MemoryContext cx)
{
	SIFT_INVARIANT(charged == NULL, "sift: a second context charged to the cache budget");
	charged = cx;
	charged_cb.func = uncharge;
	MemoryContextRegisterResetCallback(cx, &charged_cb);
}

Size
sift_cache_total_bytes(void)
{
	return (cache_root ? MemoryContextMemAllocated(cache_root, true) : 0) + (charged ? MemoryContextMemAllocated(charged, true) : 0);
}

Size
sift_cache_scan_bytes(void)
{
	return Min(sift_mem_budget_bytes(SIFT_BUDGET_WORK_MEM, 1, 1), (Size) sift_backend_cache_kb * 1024);
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

/* The kinds' oldest ticks, read once for a run of evictions: dropping an entry of one kind changes that kind's oldest alone */
typedef struct EvictRun
{
	uint64		oldest[SIFT_CACHE_MAX_KINDS];
	Size		total;			/* sift_cache_total_bytes() after the last eviction */
} EvictRun;

static void
evict_begin(EvictRun *run)
{
	for (int i = 0; i < nkinds; i++)
		run->oldest[i] = kinds[i]->oldest();
	run->total = sift_cache_total_bytes();
}

static bool
evict_oldest(EvictRun *run, uint64 keep_from)
{
	int			victim = -1;
	uint64		best = UINT64_MAX;
	Size		after;

	for (int i = 0; i < nkinds; i++)
		if (run->oldest[i] < best)
		{
			best = run->oldest[i];
			victim = i;
		}
	if (victim < 0 || best >= keep_from || !kinds[victim]->evict_one())
		return false;
	run->oldest[victim] = kinds[victim]->oldest();
	after = sift_cache_total_bytes();
	reclaimed += run->total - Min(run->total, after);
	run->total = after;
	return true;
}

static void
trim_reclaimed(void)
{
	if (reclaimed < CACHE_TRIM_BYTES)
		return;
	sift_mem_trim_os();
	reclaimed = 0;
}

/* Drops the oldest unpinned entries used before keep_from until need more bytes fit the limit */
static bool
make_room(Size need, uint64 keep_from)
{
	Size		limit = (Size) sift_backend_cache_kb * 1024;
	bool		fits = need <= limit;
	EvictRun	run;

#ifdef SIFT_TESTING
	if (sift_cache_stress)
	{
		evict_begin(&run);
		while (evict_oldest(&run, keep_from))
			;
	}
#endif
	if (fits && sift_cache_total_bytes() + need > limit)
	{
		evict_begin(&run);
		while (fits && run.total + need > limit)
			fits = evict_oldest(&run, keep_from);
	}
	trim_reclaimed();
	return fits;
}

bool
sift_cache_room(Size need)
{
	return make_room(need, UINT64_MAX);
}

bool
sift_cache_commit(void)
{
	return make_room(0, clock_now);
}

/* MEM[MEM-IDLE-15]: xact-end cache commit, the zstd contexts freed (their workspace is sized by the largest frame they saw) + idle malloc_trim hook (reclaims >= 128 KB) */
static void
cache_xact_event(XactEvent event, void *arg)
{
	bool		used;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE)
		return;
	sift_colz_thread_fini();
	used = clock_now != xact_floor;
	xact_floor = clock_now;
	if (cache_root == NULL || !used)
		return;
	sift_cache_room(0);
	sift_mem_trim_if_free(CACHE_IDLE_TRIM_FREE_BYTES);
}

static void
evict_all_kinds(void)
{
	Size		before = sift_cache_total_bytes();

	for (int i = 0; i < nkinds; i++)
		kinds[i]->evict_all();
	reclaimed += before - Min(before, sift_cache_total_bytes());
}

void
sift_cache_discard_all(void)
{
	if (cache_root == NULL)
		return;
	evict_all_kinds();
	sift_mem_trim_os();
	reclaimed = 0;
}

static void
backend_cache_assign(int newval, void *extra)
{
	EvictRun	run;

	if (cache_root == NULL)
		return;
	if (newval == 0)
		evict_all_kinds();
	else
	{
		evict_begin(&run);
		while (run.total > (Size) newval * 1024 && evict_oldest(&run, UINT64_MAX))
			;
	}
	trim_reclaimed();
}

void
sift_cache_budget_define_gucs(void)
{
	sift_xact_callback(cache_xact_event, NULL);
	DefineCustomIntVariable("sift.backend_cache_kb",
							"Memory per backend that every cache of the extension keeps together for the next statement (0: no cache is kept).",
							"The least recently used entry that no statement holds is dropped from whichever cache has it when another needs room; an entry that alone is larger than the total is not kept.",
							&sift_backend_cache_kb, 32 * 1024, 0, MAX_KILOBYTES,
							PGC_SUSET, GUC_UNIT_KB, NULL, backend_cache_assign, NULL);
}
