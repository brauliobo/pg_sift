/*
 * col/f2/f2_scope_pairs.c - the pair map of a scope kept by the backend for the statements that follow. A summary read for rows it does not hold needs the pairs to tell which keys a row
 * counts for; while the journals say the pairs are those the object was built from, the map of one object is the map of every statement that reads it, so it is read from the pair
 * relation once, until the object is replaced or the budget of the backend caches asks for the memory.
 */
/* check-memory: allow-allocs the arrays follow the pairs of the scope relation, which the build holds in memory by design */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "utils/fmgroids.h"
#include "catalog/pg_type.h"
#include "utils/memutils.h"

#include "col/scope.h"

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

static Oid
int_eq_proc(Oid type)
{
	return type == INT2OID ? F_INT2EQ : type == INT4OID ? F_INT4EQ : F_INT8EQ;
}

static Datum
int_datum(Oid type, int64 v)
{
	return type == INT2OID ? Int16GetDatum((int16) v) : type == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

static int
index_position(const Relation ix, AttrNumber att)
{
	for (int i = 0; i < ix->rd_index->indnkeyatts; i++)
		if (ix->rd_index->indkey.values[i] == att)
			return i + 1;
	elog(ERROR, "the pairs of the scope are read through a btree that does not have column %d", att);
}

/* The pairs of a scope of two paths for the changed members and the keys asked: the pairs of both paths now, as the snapshot sees them, cut to those */
static SiftScopePairMap *
edited_two_paths(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged)
{
	SiftScopePairMap *all = sift_f2s_load_pairs(heap, rel, sc, snap);
	SiftScopeMemberKey *now = palloc(sizeof(SiftScopeMemberKey) * Max(nchanged * nkeys, 1));
	uint32		n = 0;
	SiftScopePairMap *pm;

	for (uint32 i = 0; i < nchanged; i++)
	{
		uint32		lo,
					hi;

		if (!sift_spm_find(all, changed[i], &lo, &hi))
			continue;
		for (uint32 k = 0; k < nkeys; k++)
			for (uint32 j = lo; j < hi; j++)
				if (all->scope_keys[all->key_ordinals[j]] == keys[k])
				{
					now[n++] = (SiftScopeMemberKey) {changed[i], keys[k]};
					break;
				}
	}
	pm = sift_spm_patched(object, changed, nchanged, now, n);
	pfree(now);
	sift_spm_free(all);
	return pm;
}

/*
 * The pairs of the changed members (ascending, distinct) that are now, for the scope keys the read asks for: each (member, key) is looked up in the btree that defines the pairs, as the snapshot sees it,
 * and the map of the object has them replaced (sift_spm_patched). Every other pair of the map is as it was written, because the journal names every member whose pairs changed.
 */
static SiftScopePairMap *
edited_one_path(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged)
{
	Relation	prel;
	AttrNumber	a,
				b;
	Relation	ix = index_open(sift_scope_relation_index(heap, rel, sc->spec, &prel, &a, &b), AccessShareLock);
	const Oid	ta = TupleDescAttr(prel->rd_att, a - 1)->atttypid,
				tb = TupleDescAttr(prel->rd_att, b - 1)->atttypid;
	const int	pa = index_position(ix, a),
				pb = index_position(ix, b);
	TupleTableSlot *slot = table_slot_create(prel, NULL);
	IndexScanDesc scan = sift_index_beginscan(prel, ix, snap, 2);
	SiftScopeMemberKey *now = palloc(sizeof(SiftScopeMemberKey) * Max(nchanged * nkeys, 1));
	uint32		n = 0;
	SiftScopePairMap *pm;

	for (uint32 i = 0; i < nchanged; i++)
		for (uint32 k = 0; k < nkeys; k++)
		{
			ScanKeyData sk[2];

			ScanKeyInit(&sk[0], pa, BTEqualStrategyNumber, int_eq_proc(ta), int_datum(ta, changed[i]));
			ScanKeyInit(&sk[1], pb, BTEqualStrategyNumber, int_eq_proc(tb), int_datum(tb, keys[k]));
			index_rescan(scan, sk, 2, NULL, 0);
			if (index_getnext_slot(scan, ForwardScanDirection, slot))
				now[n++] = (SiftScopeMemberKey) {changed[i], keys[k]};
		}
	pm = sift_spm_patched(object, changed, nchanged, now, n);
	pfree(now);
	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(ix, NoLock);
	table_close(prel, NoLock);
	return pm;
}

SiftScopePairMap *
sift_f2s_edited_pairs(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged)
{
	return sc->spec->joined != NULL ? edited_two_paths(heap, rel, sc, snap, object, keys, nkeys, changed, nchanged) : edited_one_path(heap, rel, sc, snap, object, keys, nkeys, changed, nchanged);
}
