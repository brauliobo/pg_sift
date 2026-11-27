/*
 * test/unit/bm25/segcache_huge.c - an entry of the segment cache (bm25/segcache.c) past 1 GB, which sift.backend_cache_kb allows: it is made with the
 * huge allocation, as a segment read that the cache does not keep is ("invalid memory alloc request size" for a cached entry of more than 1 GB).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/segcache.c"

#define ENTRY_BYTES	(MaxAllocSize + 1)

int			sift_backend_cache_kb = (int) (8 * (ENTRY_BYTES / 1024));

static CacheEnt ent;

MemoryContext
sift_cache_create_context(const char *name)
{
	return CurrentMemoryContext;
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
	return (HTAB *) &ent;
}

void *
hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action, bool *foundPtr)
{
	return action == HASH_ENTER ? &ent : NULL;
}

void
MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
}

static void
get_entry(void *arg)
{
	SiftStreamReader *sr = arg;
	void	   *obj,
			   *handle;

	sift_segcache_get(sr, 0, 1, ENTRY_BYTES, CurrentMemoryContext, &obj, &handle);
	SIFT_CHECKF(handle == &ent && obj != NULL, "the entry of %zu bytes was not cached", (size_t) ENTRY_BYTES);
}

int
main(void)
{
	RelationData index = {0};
	SiftStreamReader sr = {.index = &index};

	SIFT_CHECKF(pgshim_try(get_entry, &sr), "a cached entry of %zu bytes failed: %s", (size_t) ENTRY_BYTES, pgshim_errmsg);
	return sift_check_summary();
}
