/*
 * table/f2/f2_gcache.c - the point cache of a sift_table (f2_gcache.h) and the lookup of a row by address through it.
 *
 * check-memory: allow-allocs the cache holds at most SIFT_F2_GCACHE_MAX_GRANULES granules and SIFT_F2_GCACHE_MAX_FRAMES frames, and the budget of core/cachebudget.h bounds what they hold
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
	if (sift_backend_cache_kb == 0 || pin->uncached || hash_get_num_entries(gcache) >= SIFT_F2_GCACHE_MAX_GRANULES)
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

/* ---- the frames of the framed chunks of the cached granules ---- */

typedef struct FKey
{
	GKey		granule;
	uint32		col;
	uint32		frame;
} FKey;

typedef struct FEntry
{
	FKey		key;
	uint32		relhash;
	uint64		tick;
	uint32	   *off;			/* strings: SIFT_FRAME_ROWS + 1 offsets into bytes */
	char	   *bytes;
	double	   *f8;				/* doubles: SIFT_FRAME_ROWS values */
	dlist_node	lru;
} FEntry;

static HTAB *fcache = NULL;
static MemoryContext fcache_cx = NULL;
static dlist_head fcache_lru = DLIST_STATIC_INIT(fcache_lru);

static void
fdrop_entry(FEntry *e)
{
	FKey		key = e->key;

	dlist_delete(&e->lru);
	if (e->bytes != NULL)
	{
		pfree(e->off);
		pfree(e->bytes);
	}
	if (e->f8 != NULL)
		pfree(e->f8);
	hash_search(fcache, &key, HASH_REMOVE, NULL);
}

static FEntry *
fvictim(void)
{
	return dlist_is_empty(&fcache_lru) ? NULL : dlist_container(FEntry, lru, dlist_head_node(&fcache_lru));
}

static uint64
fcache_oldest(void)
{
	FEntry	   *e = fvictim();

	return e != NULL ? e->tick : UINT64_MAX;
}

static bool
fcache_evict_one(void)
{
	FEntry	   *e = fvictim();

	if (e == NULL)
		return false;
	fdrop_entry(e);
	return true;
}

static void
fcache_evict_all(void)
{
	while (fcache_evict_one())
		;
}

static void
fcache_invalidate(uint32 hashvalue)
{
	dlist_mutable_iter it;

	if (fcache == NULL)
		return;
	dlist_foreach_modify(it, &fcache_lru)
	{
		FEntry	   *e = dlist_container(FEntry, lru, it.cur);

		if (sift_f2_inval_hits(e->relhash, hashvalue))
			fdrop_entry(e);
	}
}

static const SiftCacheKind fcache_kind = {"table_f2_frame_cache", fcache_oldest, fcache_evict_one, fcache_evict_all};

static void
fcache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(FKey),.entrysize = sizeof(FEntry)};

	fcache_cx = sift_cache_create_context("sift cache f2 table frames");
	ctl.hcxt = fcache_cx;
	fcache = hash_create("sift cache f2 table frames", SIFT_F2_GCACHE_MAX_FRAMES * 2, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&fcache_kind);
	sift_f2_invalidator_register(fcache_invalidate);
}

/* The frame of column phys that the row of the granule lies in, from the cache or inflated now (in the context of the cache); NULL when the column of the row is not in a frame */
static FEntry *
frame_get(Relation rel, const SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, uint16 row, uint16 *within)
{
	FKey		key;
	FEntry	   *e;
	FEntry		made = {0};
	SiftFrameRef ref;
	MemoryContext old;

	if (sift_backend_cache_kb == 0 || !sift_granule_frame_ref(&gb->view, phys, row, &ref))
		return NULL;
	if (fcache == NULL)
		fcache_init();
	memset(&key, 0, sizeof(key));
	key.granule.locator = rel->rd_locator;
	key.granule.part_gen = gb->pin->part_gen;
	key.granule.header_blk = gb->pin->desc.header_blk;
	key.granule.granule_id = gb->granule_id;
	key.col = phys;
	key.frame = ref.frame;
	*within = ref.within;
	e = hash_search(fcache, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		e->tick = sift_cache_tick();
		dlist_move_tail(&fcache_lru, &e->lru);
		return e;
	}
	old = MemoryContextSwitchTo(fcache_cx);
	if (kind == SIFT_CELL_F8)
	{
		made.f8 = palloc(SIFT_FRAME_ROWS * sizeof(double));
		sift_granule_frame_f8(&gb->view, phys, ref.frame, made.f8);
	}
	else
		sift_granule_frame_str(&gb->view, phys, ref.frame, &made.off, &made.bytes);
	MemoryContextSwitchTo(old);
	while (hash_get_num_entries(fcache) >= SIFT_F2_GCACHE_MAX_FRAMES && fcache_evict_one())
		;
	e = hash_search(fcache, &key, HASH_ENTER, NULL);
	made.key = key;
	made.relhash = sift_f2_rel_hash(rel);
	made.tick = sift_cache_tick();
	*e = made;
	dlist_push_tail(&fcache_lru, &e->lru);
	sift_cache_commit();
	return e;
}

bool
sift_f2_gcache_cell_str(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, SiftStrCell *cell)
{
	uint16		within;
	FEntry	   *e = frame_get(rel, gb, phys, SIFT_CELL_STR, row, &within);

	if (e == NULL)
		return false;
	*cell = (SiftStrCell) {.p = e->bytes + e->off[within],.len = e->off[within + 1] - e->off[within]};
	return true;
}

bool
sift_f2_gcache_cell_f8(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, double *value)
{
	uint16		within;
	FEntry	   *e = frame_get(rel, gb, phys, SIFT_CELL_F8, row, &within);

	if (e == NULL)
		return false;
	*value = e->f8[within];
	return true;
}

void
sift_f2_gcache_forget(const RelFileLocator *locator)
{
	dlist_mutable_iter it;

	if (gcache != NULL)
		dlist_foreach_modify(it, &gcache_lru)
		{
			GEntry	   *e = dlist_container(GEntry, lru, it.cur);

			if (RelFileLocatorEquals(e->key.locator, *locator))
				drop_entry(e);
		}
	if (fcache != NULL)
		dlist_foreach_modify(it, &fcache_lru)
		{
			FEntry	   *e = dlist_container(FEntry, lru, it.cur);

			if (RelFileLocatorEquals(e->key.granule.locator, *locator))
				fdrop_entry(e);
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
