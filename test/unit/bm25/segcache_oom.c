/*
 * test/unit/bm25/segcache_oom.c - an allocation that fails inside sift_segcache_get (bm25/segcache.c) leaves the cache as it was: an entry the failed
 * call made or took from the LRU stays evictable (it was left in the pinned list with no pin, never evicted again), and the bytes of an entry the hash
 * could not take are not left behind in the cache's memory.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

static void *test_alloc(MemoryContext c, Size n);
static void *test_alloc_aligned(MemoryContext c, Size n, Size align, int flags);
static void test_pfree(void *p);

#define MemoryContextAlloc			test_alloc
#define MemoryContextAllocAligned	test_alloc_aligned
#define pfree						test_pfree
#include "bm25/segcache.c"
#undef MemoryContextAlloc
#undef MemoryContextAllocAligned
#undef pfree

#define ENTRY_BYTES	4096
#define NSLOT		8

int			sift_backend_cache_kb = 64;

static CacheEnt slots[NSLOT];
static bool slot_used[NSLOT];
static MemoryContextCallback *last_cb;
static bool fail_pin, fail_enter, fail_bytes;
static uint64 entry_id;
static void *cache_ptr[NSLOT];

MemoryContext
sift_cache_create_context(const char *name)
{
	return (MemoryContext) 2;
}

void
sift_cache_register(const SiftCacheKind *kind)
{
}

bool
sift_cache_room(Size need)
{
	return true;
}

bool
sift_cache_commit(void)
{
	return true;
}

uint64
sift_cache_tick(void)
{
	return 1;
}

HTAB *
hash_create(const char *tabname, long nelem, const HASHCTL *info, int flags)
{
	return (HTAB *) slots;
}

void *
hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action, bool *foundPtr)
{
	int			free_slot = -1;

	for (int i = 0; i < NSLOT; i++)
		if (slot_used[i] && memcmp(&slots[i].key, keyPtr, sizeof(CacheKey)) == 0)
		{
			slot_used[i] = action != HASH_REMOVE;
			return &slots[i];
		}
		else if (!slot_used[i] && free_slot < 0)
			free_slot = i;
	if (action != HASH_ENTER)
		return NULL;
	if (fail_enter)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory: hash entry")));
	slot_used[free_slot] = true;
	memcpy(&slots[free_slot].key, keyPtr, sizeof(CacheKey));
	return &slots[free_slot];
}

void
MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
	last_cb = cb;
}

static void *
test_alloc(MemoryContext c, Size n)
{
	if (fail_pin)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory: pin")));
	return MemoryContextAlloc(c, n);
}

static void *
test_alloc_aligned(MemoryContext c, Size n, Size align, int flags)
{
	if (c == cache_cx && fail_bytes)
	{
		if (flags & MCXT_ALLOC_NO_OOM)
			return NULL;
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory: entry bytes")));
	}
	void	   *p = MemoryContextAllocAligned(c, n, align, flags);

	for (int i = 0; c == cache_cx && i < NSLOT; i++)
		if (cache_ptr[i] == NULL)
		{
			cache_ptr[i] = p;
			break;
		}
	return p;
}

static void
test_pfree(void *p)
{
	for (int i = 0; i < NSLOT; i++)
		if (cache_ptr[i] == p)
			cache_ptr[i] = NULL;
	pfree(p);
}

static int
entries(void)
{
	int			n = 0;

	for (int i = 0; i < NSLOT; i++)
		n += slot_used[i];
	return n;
}

static int
cache_live(void)
{
	int			n = 0;

	for (int i = 0; i < NSLOT; i++)
		n += cache_ptr[i] != NULL;
	return n;
}

static void
get(void *arg)
{
	void	   *obj,
			   *handle;

	sift_segcache_get(arg, 0, entry_id, ENTRY_BYTES, CurrentMemoryContext, &obj, &handle);
	sift_segcache_ready(handle);
}

/* The reader's context goes: its pin is released */
static void
release(void)
{
	last_cb->func(last_cb->arg);
	last_cb = NULL;
}

/* After the failure, every entry is evictable: evicting all leaves nothing cached */
static void
check_clean(const char *what)
{
	entry_id++;
	ent_evict_all();
	SIFT_CHECKF(cache_bytes == 0 && entries() == 0 && dlist_is_empty(&pinned) && cache_live() == 0,
				"%s: %zu bytes, %d entries, %d live allocations left after evicting all", what, (size_t) cache_bytes, entries(),
				cache_live());
}

int
main(void)
{
	RelationData index = {0};
	SiftStreamReader sr = {.index = &index};

	fail_pin = true;
	SIFT_CHECKF(!pgshim_try(get, &sr), "the failed allocation did not fail the read");
	fail_pin = false;
	check_clean("a new entry whose pin failed");

	SIFT_CHECKF(pgshim_try(get, &sr), "a cached read failed: %s", pgshim_errmsg);
	release();
	fail_pin = true;
	SIFT_CHECKF(!pgshim_try(get, &sr), "the failed allocation did not fail the read");
	fail_pin = false;
	check_clean("an LRU entry whose pin failed");

	fail_enter = true;
	SIFT_CHECKF(!pgshim_try(get, &sr), "the failed allocation did not fail the read");
	fail_enter = false;
	check_clean("a new entry the hash could not take");

	fail_bytes = true;
	SIFT_CHECKF(pgshim_try(get, &sr), "a read the cache had no memory for failed: %s", pgshim_errmsg);
	fail_bytes = false;
	SIFT_CHECKF(last_cb == NULL, "a read the cache did not keep holds a pin");
	check_clean("a new entry the cache had no memory for");
	pgshim_reset();
	return sift_check_summary();
}
