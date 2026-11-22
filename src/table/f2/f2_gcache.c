/*
 * table/f2/f2_gcache.c - the point cache of a sift_table (f2_gcache.h) and the lookup of a row by address through it.
 *
 * check-memory: allow-allocs the cache holds at most SIFT_F2_GCACHE_MAX_GRANULES granules and the budget of core/cachebudget.h bounds what they hold
 */
#include "postgres.h"

#include "utils/hsearch.h"
#include "utils/inval.h"
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
	Oid			relid;
	SiftF2GranuleBuf *gb;
	uint64		tick;
} GEntry;

#define GCACHE_FLOOR_BYTES	((Size) 8 << 20)

static HTAB *gcache = NULL;
static MemoryContext gcache_cx = NULL;

static GEntry *
victim(void)
{
	HASH_SEQ_STATUS seq;
	GEntry	   *e,
			   *best = NULL;

	if (gcache == NULL)
		return NULL;
	hash_seq_init(&seq, gcache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (e->gb->refcnt == 1 && (best == NULL || e->tick < best->tick))
			best = e;
	return best;
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
	GKey		key;

	if (e == NULL)
		return false;
	key = e->key;
	sift_f2_gbuf_unref(e->gb);
	hash_search(gcache, &key, HASH_REMOVE, NULL);
	return true;
}

static void
gcache_evict_all(void)
{
	while (gcache_evict_one())
		;
}

static void
gcache_invalidate(Datum arg pg_attribute_unused(), Oid relid)
{
	HASH_SEQ_STATUS seq;
	GEntry	   *e;

	if (gcache == NULL)
		return;
	hash_seq_init(&seq, gcache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (!OidIsValid(relid) || e->relid == relid)
		{
			sift_f2_gbuf_unref(e->gb);
			hash_search(gcache, &e->key, HASH_REMOVE, NULL);
		}
}

static const SiftCacheKind gcache_kind = {"table_f2_point_cache", gcache_oldest, gcache_evict_one, gcache_evict_all};

static void
gcache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(GKey),.entrysize = sizeof(GEntry)};

	gcache_cx = sift_cache_create_context("sift cache f2 table granules");
	ctl.hcxt = gcache_cx;
	gcache = hash_create("sift cache f2 table granules", SIFT_F2_GCACHE_MAX_GRANULES * 2, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&gcache_kind);
	CacheRegisterRelcacheCallback(gcache_invalidate, (Datum) 0);
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
	e = hash_search(gcache, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		e->tick = sift_cache_tick();
		return sift_f2_gbuf_ref(e->gb);
	}
	gb = sift_f2_gbuf_load_lazy(rel, store, pin, g, CurrentMemoryContext);
	while (hash_get_num_entries(gcache) >= SIFT_F2_GCACHE_MAX_GRANULES && gcache_evict_one())
		;
	if (hash_get_num_entries(gcache) >= SIFT_F2_GCACHE_MAX_GRANULES)
	{
		MemoryContextSetParent(gb->cx, TopTransactionContext);
		return gb;
	}
	MemoryContextSetParent(gb->cx, gcache_cx);
	gb->cached = true;
	e = hash_search(gcache, &key, HASH_ENTER, NULL);
	e->gb = sift_f2_gbuf_ref(gb);
	e->relid = RelationGetRelid(rel);
	e->tick = sift_cache_tick();
	return gb;
}

void
sift_f2_gcache_charge(void)
{
	Size		limit = sift_backend_cache_kb == 0 ? 0 : Max(GCACHE_FLOOR_BYTES, (Size) sift_backend_cache_kb * 1024 / 2);

	while (gcache != NULL && MemoryContextMemAllocated(gcache_cx, true) > limit && gcache_evict_one())
		;
}

bool
sift_f2_locate_cached(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	return sift_f2_locate_via(rel, store, tid, sift_f2_gcache_get, loc);
}
