/*
 * col/f2/f2_scope_pairs.c - the pair map of a scope kept by the backend for the statements that follow. A summary read for rows it does not hold needs the pairs to tell which keys a row
 * counts for; while the journals say the pairs are those the object was built from, the map of one object is the map of every statement that reads it, so it is read from the pair
 * relation once, until the object is replaced or the budget of the backend caches asks for the memory.
 */
/* check-memory: allow-allocs the arrays follow the pairs of the scope relation, which the build holds in memory by design */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/f2_scope_int.h"
#include "core/cachebudget.h"

typedef struct PairsCache
{
	Oid			index;
	RelFileNumber relnumber;
	uint64		gen;
	uint64		used;
	MemoryContext cx;
	SiftScopePairMap *pm;
	int			pins;
} PairsCache;

/* A statement reading the map holds it, so that the budget does not drop it under the read; the memory context of the read releases it, an error included */
typedef struct PairsPin
{
	MemoryContextCallback cb;
} PairsPin;

static PairsCache cache;
static bool registered;

static void
cache_drop(void)
{
	if (cache.cx != NULL)
		MemoryContextDelete(cache.cx);
	cache = (PairsCache) {0};
}

static uint64
cache_oldest(void)
{
	return cache.pm != NULL && cache.pins == 0 ? cache.used : UINT64_MAX;
}

static bool
cache_evict_one(void)
{
	if (cache.pm == NULL || cache.pins > 0)
		return false;
	cache_drop();
	return true;
}

static void
cache_evict_all(void)
{
	cache_evict_one();
}

static void
pin_release(void *arg)
{
	cache.pins--;
}

static const SiftCacheKind cache_kind = {"scope_pairs", cache_oldest, cache_evict_one, cache_evict_all};

static SiftScopePairMap *
copy_map(const SiftScopePairMap *pm, MemoryContext cx)
{
	SiftScopePairMap *c = MemoryContextAlloc(cx, sizeof(*c));

	*c = *pm;
	c->member_ids = MemoryContextAlloc(cx, sizeof(int64) * Max(pm->npairs, 1));
	c->offsets = MemoryContextAlloc(cx, sizeof(uint32) * (pm->nmembers + 1));
	c->key_ordinals = MemoryContextAlloc(cx, sizeof(uint32) * Max(pm->npairs, 1));
	c->scope_keys = MemoryContextAlloc(cx, sizeof(int64) * Max(pm->nkeys, 1));
	memcpy(c->member_ids, pm->member_ids, sizeof(int64) * pm->nmembers);
	memcpy(c->offsets, pm->offsets, sizeof(uint32) * (pm->nmembers + 1));
	memcpy(c->key_ordinals, pm->key_ordinals, sizeof(uint32) * pm->npairs);
	memcpy(c->scope_keys, pm->scope_keys, sizeof(int64) * pm->nkeys);
	c->cursor = 0;
	return c;
}

SiftScopePairMap *
sift_f2s_object_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, uint64 object_gen, MemoryContext reader)
{
	SiftScopePairMap *loaded;
	bool		same = cache.pm != NULL && cache.index == RelationGetRelid(index) && cache.relnumber == index->rd_locator.relNumber && cache.gen == object_gen;
	PairsPin   *pin;

	if (!registered)
	{
		sift_cache_register(&cache_kind);
		registered = true;
	}
	if (!same && cache.pins > 0)
		return sift_f2s_load_pairs(heap, index, sc, snap);
	if (!same)
	{
		cache_drop();
		loaded = sift_f2s_load_pairs(heap, index, sc, snap);
		cache.cx = sift_cache_create_context("sift cache scope pairs");
		cache.pm = copy_map(loaded, cache.cx);
		cache.index = RelationGetRelid(index);
		cache.relnumber = index->rd_locator.relNumber;
		cache.gen = object_gen;
		sift_spm_free(loaded);
	}
	cache.used = sift_cache_tick();
	pin = MemoryContextAlloc(reader, sizeof(PairsPin));
	pin->cb = (MemoryContextCallback) {pin_release, NULL, NULL};
	MemoryContextRegisterResetCallback(reader, &pin->cb);
	cache.pins++;
	return cache.pm;
}
