/*
 * table/f2/f2_gcache.c - the point cache of a sift_table (f2_gcache.h) and the lookup of a row by address through it.
 *
 * check-memory: allow-allocs the cache holds at most SIFT_F2_GCACHE_MAX_GRANULES granules and the budget of core/cachebudget.h bounds what they hold
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "table/f2/f2_gcache.h"

typedef struct GKey
{
	RelFileLocator locator;
	uint64		part_gen;
	BlockNumber header_blk;
	uint32		granule_id;
	uint32		schema_hash;
} GKey;

typedef struct GEntry
{
	GKey		key;
	uint32		relhash;		/* sift_f2_rel_hash of the relation, taken when the granule is cached */
	SiftF2GranuleBuf *gb;
	uint64		tick;
	dlist_node	lru;			/* the least recently used first: the order of the ticks */
} GEntry;

static HTAB *gcache = NULL;
static MemoryContext gcache_cx = NULL;
static dlist_head gcache_lru = DLIST_STATIC_INIT(gcache_lru);

/* The least recently used granule that no scan holds */
static GEntry *
victim(void)
{
	dlist_iter	it;

	dlist_foreach(it, &gcache_lru)
	{
		GEntry	   *e = dlist_container(GEntry, lru, it.cur);

		if (e->gb->refcnt == 1)
			return e;
	}
	return NULL;
}

static void
drop_entry(GEntry *e)
{
	GKey		key = e->key;

	dlist_delete(&e->lru);
	sift_f2_gbuf_unref(e->gb);
	hash_search(gcache, &key, HASH_REMOVE, NULL);
}

static uint64
gcache_oldest(void)
{
	GEntry	   *e = victim();

	return e != NULL ? e->tick : UINT64_MAX;
}

static bool
gcache_evict_one(void)
{
	GEntry	   *e = victim();

	if (e == NULL)
		return false;
	drop_entry(e);
	return true;
}

static void
gcache_evict_all(void)
{
	while (gcache_evict_one())
		;
}

static void
gcache_invalidate(uint32 hashvalue)
{
	dlist_mutable_iter it;

	if (gcache == NULL)
		return;
	dlist_foreach_modify(it, &gcache_lru)
	{
		GEntry	   *e = dlist_container(GEntry, lru, it.cur);

		if (sift_f2_inval_hits(e->relhash, hashvalue))
			drop_entry(e);
	}
}

static const SiftCacheKind gcache_kind = {"table_f2_point_cache", gcache_oldest, gcache_evict_one, gcache_evict_all};

/* MEM[MEM-TBL-08]: cachebudget f2 table granule cache (no 8 MiB floor; calls sift_cache_commit) */
static void
gcache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(GKey),.entrysize = sizeof(GEntry)};

	gcache_cx = sift_cache_create_context("sift cache f2 table granules");
	ctl.hcxt = gcache_cx;
	gcache = hash_create("sift cache f2 table granules", SIFT_F2_GCACHE_MAX_GRANULES * 2, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&gcache_kind);
	sift_f2_invalidator_register(gcache_invalidate);
}

SiftF2GranuleBuf *
sift_f2_gcache_get(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g)
{
	GKey		key;
	GEntry	   *e;
	SiftF2GranuleBuf *gb;

	if (gcache == NULL)
		gcache_init();
	memset(&key, 0, sizeof(key));
	key.locator = rel->rd_locator;
	key.part_gen = pin->part_gen;
	key.header_blk = pin->desc.header_blk;
	key.granule_id = g;
	key.schema_hash = store->schema_hash;
	e = pin->uncached ? NULL : hash_search(gcache, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		e->tick = sift_cache_tick();
		dlist_move_tail(&gcache_lru, &e->lru);
		return sift_f2_gbuf_ref(e->gb);
	}
	gb = sift_f2_gbuf_load_lazy(rel, store, pin, g, CurrentMemoryContext);
	while (sift_backend_cache_kb != 0 && hash_get_num_entries(gcache) >= SIFT_F2_GCACHE_MAX_GRANULES && gcache_evict_one())
		;
	if (pin->uncached || sift_backend_cache_kb == 0 || hash_get_num_entries(gcache) >= SIFT_F2_GCACHE_MAX_GRANULES)
	{
		MemoryContextSetParent(gb->cx, TopTransactionContext);
		return gb;
	}
	MemoryContextSetParent(gb->cx, gcache_cx);
	gb->cached = true;
	e = hash_search(gcache, &key, HASH_ENTER, NULL);
	e->gb = sift_f2_gbuf_ref(gb);
	e->relhash = sift_f2_rel_hash(rel);
	e->tick = sift_cache_tick();
	dlist_push_tail(&gcache_lru, &e->lru);
	sift_cache_commit();
	return gb;
}

void
sift_f2_gcache_forget(const RelFileLocator *locator)
{
	HASH_SEQ_STATUS seq;
	GEntry	   *e;

	if (gcache == NULL)
		return;
	hash_seq_init(&seq, gcache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (RelFileLocatorEquals(e->key.locator, *locator))
		{
			sift_f2_gbuf_unref(e->gb);
			hash_search(gcache, &e->key, HASH_REMOVE, NULL);
		}
}

void
sift_f2_gcache_charge(void)
{
	Size		limit = (Size) sift_backend_cache_kb * 1024 / 2;

	if (gcache == NULL || hash_get_num_entries(gcache) == 0)
		return;
	while (MemoryContextMemAllocated(gcache_cx, true) > limit && gcache_evict_one())
		;
}

bool
sift_f2_locate_cached(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	return sift_f2_locate_via(rel, store, tid, sift_f2_gcache_get, loc);
}
