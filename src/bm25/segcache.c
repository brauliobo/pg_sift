/*
 * bm25/segcache.c - bytes of an immutable segment that queries read again and again, kept by the backend for the next one.
 *
 * A segment never changes, so what its relation file and generation hold is named for good (relation files are not reused, a
 * merge or a VACUUM that rewrites a segment names a new generation): the SB directory of its docmap (bm25/docdir.c) and the
 * blob of a term (bm25/posting/posting.c) are kept under the file, the generation, what they are, where they start and how long they
 * are, up to SEGCACHE_BYTES per backend. The reader pins the entry it uses until its memory context goes (a callback), and
 * eviction takes the least recently used one that nobody pins. What does not fit is made in the reader's memory, as before. The
 * SQLite and MariaDB hosts keep nothing (host/stubs.c).
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "bm25/segment/segment.h"
#include "core/cachebudget.h"
#include "core/limits.h"

#define SEGCACHE_BYTES		((Size) sift_backend_cache_kb * 1024)

typedef struct CacheKey
{
	RelFileLocator rel;
	uint64		gen;
	uint64		id;
	Size		size;			/* read from the segment: a damaged one must not share the entry of an intact size */
	int			kind;
} CacheKey;

typedef struct CacheEnt
{
	CacheKey	key;			/* first: the hash key */
	void	   *obj;
	int			pins;
	bool		ready;			/* filled by its first user */
	uint64		used;			/* sift_cache_tick() of the last use */
	dlist_node	node;			/* in pinned while some context holds it, else in lru, least recently released first */
} CacheEnt;

typedef struct Unpin
{
	MemoryContextCallback cb;
	CacheKey	key;
} Unpin;

#ifdef SIFT_TESTING
SiftSegCacheStats sift_segcache_stats;
#define SEGCACHE_STAT(f)	(sift_segcache_stats.f++)
#else
#define SEGCACHE_STAT(f)	((void) 0)
#endif

static HTAB *cache;
static MemoryContext cache_cx;
static Size cache_bytes;
static dlist_head lru = DLIST_STATIC_INIT(lru);
static dlist_head pinned = DLIST_STATIC_INIT(pinned);

static void
ent_drop(CacheEnt *e)
{
	cache_bytes -= e->key.size;
	dlist_delete(&e->node);
	pfree(e->obj);
	hash_search(cache, &e->key, HASH_REMOVE, NULL);
}

static void
ent_unpin(void *arg)
{
	Unpin	   *u = arg;
	CacheEnt   *e = hash_search(cache, &u->key, HASH_FIND, NULL);

	if (e && --e->pins == 0)
	{
		if (!e->ready)
			ent_drop(e);
		else
		{
			e->used = sift_cache_tick();
			dlist_delete(&e->node);
			dlist_push_tail(&lru, &e->node);
		}
	}
}

/* The least recently released entry: only entries nobody holds are in lru */
static CacheEnt *
ent_victim(void)
{
	return dlist_is_empty(&lru) ? NULL : dlist_container(CacheEnt, node, dlist_head_node(&lru));
}

static uint64
ent_oldest(void)
{
	CacheEnt   *e = ent_victim();

	return e ? e->used : UINT64_MAX;
}

static bool
ent_evict_one(void)
{
	CacheEnt   *e = ent_victim();

	if (e == NULL)
		return false;
	ent_drop(e);
	SEGCACHE_STAT(evicted);
	return true;
}

static void
ent_evict_all(void)
{
	while (ent_evict_one())
		;
}

static const SiftCacheKind seg_kind = {"bm25_segcache", ent_oldest, ent_evict_one, ent_evict_all};

/* A new entry nobody pins yet, or NULL when the cache has no room or no memory for it: the reader then makes the bytes in its own memory */
static CacheEnt *
ent_new(const CacheKey *k)
{
	CacheEnt   *e;

	if (!sift_cache_room(k->size))
		return NULL;
	e = hash_search(cache, k, HASH_ENTER, NULL);
	e->obj = MemoryContextAllocAligned(cache_cx, k->size, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);
	if (e->obj == NULL)
	{
		hash_search(cache, k, HASH_REMOVE, NULL);
		return NULL;
	}
	e->pins = 0;
	e->ready = false;
	dlist_push_tail(&pinned, &e->node);
	cache_bytes += k->size;
	sift_cache_commit();
	return e;
}

/* MEM[MEM-BM25-03]: segcache entry (MCXT_ALLOC_HUGE, missing sift_cache_commit; 256-entry HTAB) */
bool
sift_segcache_get(const SiftStreamReader *sr, int kind, uint64 id, Size size, MemoryContext mcxt, void **obj, void **handle)
{
	CacheKey	k;
	CacheEnt   *e;
	Unpin	   *u;

	*handle = NULL;
	if (size <= SEGCACHE_BYTES / 4)
	{
		if (cache == NULL)
		{
			HASHCTL		ctl = {.keysize = sizeof(CacheKey),.entrysize = sizeof(CacheEnt)};

			cache_cx = sift_cache_create_context("sift cache segments");
			ctl.hcxt = cache_cx;
			cache = hash_create("sift segment bytes", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			sift_cache_register(&seg_kind);
		}
		memset(&k, 0, sizeof(k));
		k.rel = sr->index->rd_locator;
		k.gen = sr->gen;
		k.id = id;
		k.size = size;
		k.kind = kind;
		/* the pin is made first: once an entry is pinned, nothing fails before its callback unpins it */
		u = MemoryContextAlloc(mcxt, sizeof(Unpin));
		e = hash_search(cache, &k, HASH_FIND, NULL);
		if (e == NULL)
			e = ent_new(&k);
		else if (e->pins == 0)
		{
			dlist_delete(&e->node);
			dlist_push_tail(&pinned, &e->node);
		}
		if (e)
		{
			e->used = sift_cache_tick();
			e->pins++;
			u->cb = (MemoryContextCallback) {ent_unpin, u, NULL};
			u->key = k;
			MemoryContextRegisterResetCallback(mcxt, &u->cb);
			*obj = e->obj;
			*handle = e;
			return e->ready;
		}
		pfree(u);
	}
	SEGCACHE_STAT(refused);
	*obj = MemoryContextAllocAligned(mcxt, size, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	return false;
}

bool
sift_segcache_has(const SiftStreamReader *sr, int kind, uint64 id, Size size)
{
	CacheKey	k;
	CacheEnt   *e;

	if (cache == NULL)
		return false;
	memset(&k, 0, sizeof(k));
	k.rel = sr->index->rd_locator;
	k.gen = sr->gen;
	k.id = id;
	k.size = size;
	k.kind = kind;
	e = hash_search(cache, &k, HASH_FIND, NULL);
	return e != NULL && e->ready;
}

void
sift_segcache_ready(void *handle)
{
	if (handle)
		((CacheEnt *) handle)->ready = true;
}

#ifdef SIFT_TESTING
Size
sift_segcache_bytes(void)
{
	return cache_bytes;
}
#endif
