/*
 * bm25/segcache.c - bytes of an immutable segment that queries read again and again, kept by the backend for the next one.
 *
 * A segment never changes, so what its relation file and generation hold is named for good (relation files are not reused, a
 * merge or a VACUUM that rewrites a segment names a new generation): the SB directory of its docmap (bm25/docdir.c) and the
 * blob of a term (bm25/posting.c) are kept under the file, the generation, what they are, where they start and how long they
 * are, up to SEGCACHE_BYTES per backend. The reader pins the entry it uses until its memory context goes (a callback), and
 * eviction takes the least recently used one that nobody pins. What does not fit is made in the reader's memory, as before. The
 * SQLite and MariaDB hosts keep nothing (host_stubs.c).
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "bm25/segment.h"
#include "core/cachebudget.h"
#include "core/limits.h"

#define SEGCACHE_MAX_KB		32768
#define SEGCACHE_BYTES		((Size) sift_limit(segment_cache_kb, SEGCACHE_MAX_KB) * 1024)

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
	dlist_node	node;			/* in lru: least recently used first */
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

	if (e && --e->pins == 0 && !e->ready)
		ent_drop(e);
}

static CacheEnt *
ent_victim(void)
{
	dlist_iter	it;

	dlist_foreach(it, &lru)
	{
		CacheEnt   *e = dlist_container(CacheEnt, node, it.cur);

		if (e->pins == 0)
			return e;
	}
	return NULL;
}

static Size
ent_bytes(void)
{
	return cache_bytes;
}

static uint64
ent_oldest(void)
{
	CacheEnt   *e = ent_victim();

	return e ? e->used : UINT64_MAX;
}

static void
ent_evict_oldest(void)
{
	ent_drop(ent_victim());
	SEGCACHE_STAT(evicted);
}

static const SiftCacheKind seg_kind = {ent_bytes, ent_oldest, ent_evict_oldest};

/* Makes room for need bytes in this cache and the backend's budget by dropping the least recently used entries nobody pins; false when they do not fit */
static bool
ent_room(Size need)
{
	CacheEnt   *e;

	while (cache_bytes + need > SEGCACHE_BYTES && (e = ent_victim()) != NULL)
	{
		ent_drop(e);
		SEGCACHE_STAT(evicted);
	}
	return cache_bytes + need <= SEGCACHE_BYTES && sift_cache_room(need);
}

bool
sift_segcache_get(const SiftStreamReader *sr, int kind, uint64 id, Size size, MemoryContext mcxt, void **obj, void **handle)
{
	CacheKey	k;
	CacheEnt   *e;
	Unpin	   *u;

	*handle = NULL;
	if (cache_bytes > SEGCACHE_BYTES)
		ent_room(0);
	if (size <= SEGCACHE_BYTES / 4)
	{
		if (cache == NULL)
		{
			HASHCTL		ctl = {.keysize = sizeof(CacheKey),.entrysize = sizeof(CacheEnt)};

			cache_cx = AllocSetContextCreate(TopMemoryContext, "sift cache segments", ALLOCSET_DEFAULT_SIZES);
			ctl.hcxt = cache_cx;
			cache = hash_create("sift segment bytes", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			sift_cache_register(&seg_kind);
		}
		memset(&k, 0, sizeof(k));
		k.rel = sr->index->rd_locator;
		k.gen = sr->gen;
		k.id = id;
		k.size = size;
		k.kind = kind;
		e = hash_search(cache, &k, HASH_FIND, NULL);
		if (e == NULL)
		{
			if (ent_room(size))
			{
				void	   *p = MemoryContextAllocAligned(cache_cx, size, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);

				e = hash_search(cache, &k, HASH_ENTER, NULL);
				e->obj = p;
				e->pins = 0;
				e->ready = false;
				e->used = sift_cache_tick();
				dlist_push_tail(&lru, &e->node);
				cache_bytes += size;
			}
			else
				SEGCACHE_STAT(refused);
		}
		else
		{
			e->used = sift_cache_tick();
			dlist_delete(&e->node);
			dlist_push_tail(&lru, &e->node);
		}
		if (e)
		{
			e->pins++;
			u = MemoryContextAlloc(mcxt, sizeof(Unpin));
			u->cb = (MemoryContextCallback) {ent_unpin, u, NULL};
			u->key = k;
			MemoryContextRegisterResetCallback(mcxt, &u->cb);
			*obj = e->obj;
			*handle = e;
			return e->ready;
		}
	}
	*obj = MemoryContextAllocAligned(mcxt, size, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	return false;
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
