/*
 * col/f2/scope/pairs.c - the pair map of a scope kept by the backend for the statements that follow. A summary read for rows it does not hold needs the pairs to tell which keys a row
 * counts for; while the journals of every key say the pairs are those the object was built from, the map of one object is the map of every statement that reads it, so it is read from the
 * pair relation once, until the object is replaced or the budget of the backend caches asks for the memory. A map read after a pair changed is not the object's and is never kept: the
 * object's pairs of the members that changed are then unknown, and the read tells who the key held from the summary's nodes.
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

#include "col/scope/scope.h"

#include "col/f2/scope/int.h"
#include "col/f2/scope/mem.h"
#include "core/cachebudget.h"
#include "core/cmp.h"

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
	/* MEM[MEM-SCOPE-01]: cached member_ids nmembers * 8 B (was npairs * 8 B: 20x at 20 keys/member) */
	c->member_ids = MemoryContextAlloc(cx, sizeof(int64) * Max(pm->nmembers, 1));
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

static Size
map_bytes(const SiftScopePairMap *pm)
{
	return sizeof(*pm) + sizeof(int64) * (Max(pm->nmembers, 1) + Max(pm->nkeys, 1)) + sizeof(uint32) * (pm->nmembers + 1 + Max(pm->npairs, 1));
}

static SiftScopePairMap *
cache_pin(MemoryContext reader)
{
	PairsPin   *pin = MemoryContextAlloc(reader, sizeof(PairsPin));

	cache.used = sift_cache_tick();
	pin->cb = (MemoryContextCallback) {pin_release, NULL, NULL};
	MemoryContextRegisterResetCallback(reader, &pin->cb);
	cache.pins++;
	(void) sift_cache_commit();
	return cache.pm;
}

SiftScopePairMap *
sift_f2s_cached_pairs(Relation index, uint64 object_gen, MemoryContext reader)
{
	const bool	same = cache.pm != NULL && cache.index == RelationGetRelid(index) && cache.relnumber == index->rd_locator.relNumber && cache.gen == object_gen;

	return same ? cache_pin(reader) : NULL;
}

/* The pairs read for reader: a read asks for them first wherever it needs them, a context it resets for each granule included */
static SiftScopePairMap *
load_for(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, MemoryContext reader)
{
	MemoryContext old = MemoryContextSwitchTo(reader);
	SiftScopePairMap *pm = sift_f2s_load_pairs(heap, index, sc, snap, km, false);

	MemoryContextSwitchTo(old);
	return pm;
}

SiftScopePairMap *
sift_f2s_object_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, uint64 object_gen, MemoryContext reader, bool settled)
{
	SiftScopePairMap *loaded;

	if (!registered)
	{
		sift_cache_register(&cache_kind);
		registered = true;
	}
	if (!settled || cache.pins > 0)
		return load_for(heap, index, sc, snap, km, reader);
	cache_drop();
	loaded = load_for(heap, index, sc, snap, km, reader);
	/* MEM[MEM-SCOPE-02]: cached pair map charged to the cache budget (room, then commit) */
	if (sift_f2s_mem_transient(sc->mem) || !sift_cache_room(map_bytes(loaded)))
		return loaded;
	cache.cx = sift_cache_create_context("sift cache scope pairs");
	cache.pm = copy_map(loaded, cache.cx);
	cache.index = RelationGetRelid(index);
	cache.relnumber = index->rd_locator.relNumber;
	cache.gen = object_gen;
	sift_spm_free(loaded);
	return cache_pin(reader);
}

static int
index_position(const Relation ix, AttrNumber att)
{
	int			i = 0;

	while (i < ix->rd_index->indnkeyatts && ix->rd_index->indkey.values[i] != att)
		i++;
	SIFT_INVARIANT(i < ix->rd_index->indnkeyatts, "sift: the pairs of the scope are read through a btree that does not have column %d", att);
	return i + 1;
}

static int
now_cmp(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

static bool
key_asked(const int64 *keys, uint32 nkeys, int64 key)
{
	return bsearch(&key, keys, nkeys, sizeof(int64), sift_cmp_i64) != NULL;
}

/* The pairs now, one entry for each (member, key), ascending: the patch of the object takes them as they are */
static SiftScopePairMap *
patch_with(const SiftScopePairMap *object, const int64 *changed, uint32 nchanged, SiftScopeMemberKey *now, uint32 n)
{
	uint32		u = 0;
	SiftScopePairMap *pm;

	qsort(now, n, sizeof(*now), now_cmp);
	for (uint32 i = 0; i < n; i++)
		if (u == 0 || now_cmp(&now[u - 1], &now[i]) != 0)
			now[u++] = now[i];
	pm = sift_spm_patched(object, changed, nchanged, now, u);
	pfree(now);
	return pm;
}

static void
now_add(SiftScopeMemberKey **now, uint32 *n, uint32 *cap, int64 member, int64 key)
{
	if (*n == *cap)
	{
		*cap = Max(*cap * 2, 64);
		*now = *now == NULL ? palloc(sizeof(**now) * *cap) : repalloc(*now, sizeof(**now) * *cap);
	}
	(*now)[(*n)++] = (SiftScopeMemberKey) {member, key};
}

/* The pairs of a scope of two paths for the changed members and the keys asked: the pairs of both paths now, as the snapshot sees them, cut to those */
static SiftScopePairMap *
edited_two_paths(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged)
{
	SiftScopePairMap *all = sift_f2s_load_pairs(heap, rel, sc, snap, km, false);
	SiftScopeMemberKey *now = NULL;
	uint32		n = 0,
				cap = 0;
	SiftScopePairMap *pm;

	for (uint32 i = 0; i < nchanged; i++)
	{
		const uint32 *ords;
		uint32		deg;

		if (sift_spm_slice(all, changed[i], &ords, &deg))
			for (uint32 j = 0; j < deg; j++)
				if (key_asked(keys, nkeys, sift_spm_key_of(all, ords[j])))
					now_add(&now, &n, &cap, changed[i], sift_spm_key_of(all, ords[j]));
	}
	pm = patch_with(object, changed, nchanged, now == NULL ? palloc(sizeof(*now)) : now, n);
	sift_spm_free(all);
	return pm;
}

/*
 * The pairs of the changed members (ascending, distinct) that are now, for the scope keys the read asks for (ascending, distinct), as the snapshot sees them in the btree that defines the pairs,
 * and the patch of the object (sift_spm_patched) has them replaced. Every other pair of the map is as it was written, because the journal names every member whose pairs changed. A btree that
 * leads with the member is read once for each changed member and its keys cut to those asked; any other is probed for each (member, key).
 */
static SiftScopePairMap *
edited_one_path(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged)
{
	Relation	prel;
	AttrNumber	a,
				b;
	Relation	ix = index_open(sift_scope_relation_index(heap, rel, sc->spec, &prel, &a, &b), AccessShareLock);
	const Oid	ta = TupleDescAttr(prel->rd_att, a - 1)->atttypid,
				tb = TupleDescAttr(prel->rd_att, b - 1)->atttypid;
	const int	pa = index_position(ix, a),
				pb = index_position(ix, b);
	const bool	by_member = pa == 1;
	SiftKeyMap *mm = sift_f2s_mem_map(sc->mem);
	TupleTableSlot *slot = table_slot_create(prel, NULL);
	IndexScanDesc scan = sift_index_beginscan(prel, ix, snap, by_member ? 1 : 2);
	SiftScopeMemberKey *now = NULL;
	uint32		n = 0,
				cap = 0;

	for (uint32 i = 0; i < nchanged; i++)
		for (uint32 k = 0; k < (by_member ? 1 : nkeys); k++)
		{
			ScanKeyData sk[2];
			ScanKey		member_sk = &sk[pa > pb],
						key_sk = &sk[pa < pb];

			sift_scope_key_eq_init(member_sk, pa, ta, TupleDescAttr(prel->rd_att, a - 1)->attcollation, sift_scope_key_datum(ta, mm, changed[i]));
			if (by_member)
			{
				index_rescan(scan, sk, 1, NULL, 0);
				while (index_getnext_slot(scan, ForwardScanDirection, slot))
				{
					bool		isnull;
					const Datum d = slot_getattr(slot, b, &isnull);
					int64		key = isnull ? 0 : sift_scope_key_code(tb, km, d);

					if (!isnull && key_asked(keys, nkeys, key))
						now_add(&now, &n, &cap, changed[i], key);
				}
				continue;
			}
			sift_scope_key_eq_init(key_sk, pb, tb, TupleDescAttr(prel->rd_att, b - 1)->attcollation, sift_scope_key_datum(tb, km, keys[k]));
			index_rescan(scan, sk, 2, NULL, 0);
			if (index_getnext_slot(scan, ForwardScanDirection, slot))
				now_add(&now, &n, &cap, changed[i], keys[k]);
		}
	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(ix, NoLock);
	table_close(prel, NoLock);
	return patch_with(object, changed, nchanged, now == NULL ? palloc(sizeof(*now)) : now, n);
}

SiftScopePairMap *
sift_f2s_edited_pairs(Relation heap, Relation rel, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km, const SiftScopePairMap *object, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged)
{
	if (object == NULL)
		return sift_f2s_load_pairs(heap, rel, sc, snap, km, false);
	return sc->spec->joined != NULL ? edited_two_paths(heap, rel, sc, snap, km, object, keys, nkeys, changed, nchanged) : edited_one_path(heap, rel, sc, snap, km, object, keys, nkeys, changed, nchanged);
}
