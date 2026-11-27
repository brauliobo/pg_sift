/*
 * table/f2/gcache.c - the point cache of a sift_table (f2_gcache.h) and the lookup of a row by address through it.
 *
 * check-memory: allow-allocs the cache holds at most SIFT_F2_GCACHE_MAX_GRANULES granules and SIFT_F2_GCACHE_MAX_FRAMES frames, and the budget of core/cachebudget.h bounds what they hold
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "table/f2/gcache.h"
#include "table/f2/row/pack.h"

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

static void invalidate_all(uint32 hashvalue);

/* The caches of a backend's fetches share one invalidator: the number of them a backend has is bounded (col/f2/store/store.h) */
static void
register_invalidator(void)
{
	static bool registered = false;

	if (registered)
		return;
	sift_f2_invalidator_register(invalidate_all);
	registered = true;
}

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
	register_invalidator();
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
	if (e != NULL && e->gb->pin == pin)
	{
		e->tick = sift_cache_tick();
		dlist_move_tail(&gcache_lru, &e->lru);
		return sift_f2_gbuf_ref(e->gb);
	}
	/* a granule decoded under an older pin of the part reads that pin's deletion vector, not the one the part has now */
	if (e != NULL)
		drop_entry(e);
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
	uint32	   *off;			/* strings: nstr + 1 offsets into bytes */
	char	   *bytes;
	uint32		nstr;
	double	   *f8;				/* doubles: SIFT_FRAME_ROWS values */
	dlist_node	lru;
} FEntry;

/* A copy of a frame a reader keeps (palloc'd in cx), which no eviction of the cache reaches */
struct SiftF2Frame
{
	MemoryContext cx;
	FKey		key;
	bool		valid;
	uint32	   *off;
	char	   *bytes;
	double	   *f8;
};

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
	register_invalidator();
}

/* The frame of column phys that the row of the granule lies in (false: the column of the row is not in a frame, or no cache is kept) and the row's place in it */
static bool
frame_key(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, FKey *key, uint16 *within)
{
	SiftFrameRef ref;

	if (sift_backend_cache_kb == 0 || !sift_granule_frame_ref(&gb->view, phys, row, &ref))
		return false;
	memset(key, 0, sizeof(*key));
	key->granule.locator = rel->rd_locator;
	key->granule.part_gen = gb->pin->part_gen;
	key->granule.header_blk = gb->pin->desc.header_blk;
	key->granule.granule_id = gb->granule_id;
	key->col = phys;
	key->frame = ref.frame;
	*within = ref.within;
	return true;
}

/* The frame from the cache or inflated now (in the context of the cache) */
static FEntry *
frame_get(Relation rel, const SiftF2GranuleBuf *gb, SiftCellKind kind, const FKey *key)
{
	FEntry	   *e;
	FEntry		made = {0};
	MemoryContext old;

	if (fcache == NULL)
		fcache_init();
	e = hash_search(fcache, key, HASH_FIND, NULL);
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
		sift_granule_frame_f8(&gb->view, key->col, key->frame, made.f8);
	}
	else
		made.nstr = (uint32) sift_granule_frame_str(&gb->view, key->col, key->frame, &made.off, &made.bytes);
	MemoryContextSwitchTo(old);
	while (hash_get_num_entries(fcache) >= SIFT_F2_GCACHE_MAX_FRAMES && fcache_evict_one())
		;
	e = hash_search(fcache, key, HASH_ENTER, NULL);
	made.key = *key;
	made.relhash = sift_f2_rel_hash(rel);
	made.tick = sift_cache_tick();
	*e = made;
	dlist_push_tail(&fcache_lru, &e->lru);
	sift_cache_commit();
	return e;
}

SiftF2Frame *
sift_f2_gcache_frame_new(MemoryContext cx)
{
	SiftF2Frame *f = MemoryContextAllocZero(cx, sizeof(SiftF2Frame));

	f->cx = cx;
	return f;
}

static void
frame_drop(SiftF2Frame *f)
{
	if (f->off != NULL)
		pfree(f->off);
	if (f->bytes != NULL)
		pfree(f->bytes);
	if (f->f8 != NULL)
		pfree(f->f8);
	f->off = NULL;
	f->bytes = NULL;
	f->f8 = NULL;
	f->valid = false;
}

void
sift_f2_gcache_frame_free(SiftF2Frame *f)
{
	frame_drop(f);
	pfree(f);
}

/* The reader's copy becomes the frame of the entry: the frame it held goes first, so that the copy takes its memory */
static void
frame_copy(SiftF2Frame *f, const FEntry *e)
{
	frame_drop(f);
	if (e->f8 != NULL)
		f->f8 = memcpy(MemoryContextAlloc(f->cx, SIFT_FRAME_ROWS * sizeof(double)), e->f8, SIFT_FRAME_ROWS * sizeof(double));
	else
	{
		f->off = memcpy(MemoryContextAlloc(f->cx, (e->nstr + 1) * sizeof(uint32)), e->off, (e->nstr + 1) * sizeof(uint32));
		f->bytes = memcpy(MemoryContextAlloc(f->cx, Max(e->off[e->nstr], 1)), e->bytes, e->off[e->nstr]);
	}
	f->key = e->key;
	f->valid = true;
}

/* Where the cell is read from: the reader's copy (copied now when it is of another frame), or the entry of the cache when the reader keeps none */
static bool
frame_of(Relation rel, const SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, uint16 row, SiftF2Frame *keep, uint16 *within, const uint32 **off, const char **bytes,
		 const double **f8)
{
	FKey		key;
	const FEntry *e;

	if (!frame_key(rel, gb, phys, row, &key, within))
		return false;
	if (keep == NULL || !keep->valid || memcmp(&keep->key, &key, sizeof(key)) != 0)
	{
		e = frame_get(rel, gb, kind, &key);
		if (keep == NULL)
		{
			*off = e->off;
			*bytes = e->bytes;
			*f8 = e->f8;
			return true;
		}
		frame_copy(keep, e);
	}
	*off = keep->off;
	*bytes = keep->bytes;
	*f8 = keep->f8;
	return true;
}

bool
sift_f2_gcache_cell_str(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, SiftF2Frame *keep, SiftStrCell *cell)
{
	uint16		within;
	const uint32 *off;
	const char *bytes;
	const double *f8;

	if (!frame_of(rel, gb, phys, SIFT_CELL_STR, row, keep, &within, &off, &bytes, &f8))
		return false;
	*cell = (SiftStrCell) {.p = bytes + off[within],.len = off[within + 1] - off[within]};
	return true;
}

bool
sift_f2_gcache_cell_f8(Relation rel, const SiftF2GranuleBuf *gb, int phys, uint16 row, SiftF2Frame *keep, double *value)
{
	uint16		within;
	const uint32 *off;
	const char *bytes;
	const double *f8;

	if (!frame_of(rel, gb, phys, SIFT_CELL_F8, row, keep, &within, &off, &bytes, &f8))
		return false;
	*value = f8[within];
	return true;
}

typedef struct BKey
{
	RelFileLocator locator;
	uint64		part_gen;
	BlockNumber header_blk;
	uint32		blk;
} BKey;

typedef struct BEntry
{
	BKey		key;
	uint32		relhash;
	uint64		tick;
	SiftRowPackBlk *block;
	dlist_node	lru;
} BEntry;

static HTAB *bcache = NULL;
static MemoryContext bcache_cx = NULL;
static SiftRowPackBlk *bunkept = NULL;
static dlist_head bcache_lru = DLIST_STATIC_INIT(bcache_lru);

static void
bdrop_entry(BEntry *e)
{
	BKey		key = e->key;

	dlist_delete(&e->lru);
	pfree(e->block);
	hash_search(bcache, &key, HASH_REMOVE, NULL);
}

static BEntry *
bvictim(void)
{
	return dlist_is_empty(&bcache_lru) ? NULL : dlist_container(BEntry, lru, dlist_head_node(&bcache_lru));
}

static uint64
bcache_oldest(void)
{
	BEntry	   *e = bvictim();

	return e != NULL ? e->tick : UINT64_MAX;
}

static bool
bcache_evict_one(void)
{
	BEntry	   *e = bvictim();

	if (e == NULL)
		return false;
	bdrop_entry(e);
	return true;
}

static void
drop_unkept(void)
{
	if (bunkept != NULL)
		pfree(bunkept);
	bunkept = NULL;
}

static void
bcache_evict_all(void)
{
	while (bcache_evict_one())
		;
	drop_unkept();
}

static void
bcache_invalidate(uint32 hashvalue)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &bcache_lru)
	{
		BEntry	   *e = dlist_container(BEntry, lru, it.cur);

		if (sift_f2_inval_hits(e->relhash, hashvalue))
			bdrop_entry(e);
	}
}

static void
invalidate_all(uint32 hashvalue)
{
	gcache_invalidate(hashvalue);
	fcache_invalidate(hashvalue);
	bcache_invalidate(hashvalue);
}

static const SiftCacheKind bcache_kind = {"table_f2_row_pack_cache", bcache_oldest, bcache_evict_one, bcache_evict_all};

static void
bcache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(BKey),.entrysize = sizeof(BEntry)};

	bcache_cx = sift_cache_create_context("sift cache f2 table row pack blocks");
	ctl.hcxt = bcache_cx;
	bcache = hash_create("sift cache f2 table row pack blocks", SIFT_F2_GCACHE_MAX_BLOCKS * 2, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&bcache_kind);
	register_invalidator();
}

static SiftRowPackBlk *
read_block_in(MemoryContext cx, Relation rel, SiftF2PartPin *pin, uint32 blk)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftRowPackBlk *b = sift_row_pack_read_block(rel, pin->part, blk);

	MemoryContextSwitchTo(old);
	return b;
}

/* The block kept for nobody: the cache keeps none (sift.backend_cache_kb 0, or the part is not in the cache), so the block is the next call's to replace */
static const SiftRowPackBlk *
block_unkept(Relation rel, SiftF2PartPin *pin, uint32 blk)
{
	drop_unkept();
	bunkept = read_block_in(bcache_cx, rel, pin, blk);
	return bunkept;
}

const SiftRowPackBlk *
sift_f2_gcache_row_pack_block(Relation rel, SiftF2PartPin *pin, uint32 blk)
{
	BKey		key;
	BEntry	   *e;
	SiftRowPackBlk *b;

	if (bcache == NULL)
		bcache_init();
	if (sift_backend_cache_kb == 0 || pin->uncached)
		return block_unkept(rel, pin, blk);
	memset(&key, 0, sizeof(key));
	key.locator = rel->rd_locator;
	key.part_gen = pin->part_gen;
	key.header_blk = pin->desc.header_blk;
	key.blk = blk;
	e = hash_search(bcache, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		e->tick = sift_cache_tick();
		dlist_move_tail(&bcache_lru, &e->lru);
		return e->block;
	}
	b = read_block_in(bcache_cx, rel, pin, blk);
	while (hash_get_num_entries(bcache) >= SIFT_F2_GCACHE_MAX_BLOCKS && bcache_evict_one())
		;
	e = hash_search(bcache, &key, HASH_ENTER, NULL);
	e->block = b;
	e->relhash = sift_f2_rel_hash(rel);
	e->tick = sift_cache_tick();
	dlist_push_tail(&bcache_lru, &e->lru);
	sift_cache_commit();
	return b;
}

void
sift_f2_gcache_forget(const RelFileLocator *locator)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &gcache_lru)
	{
		GEntry	   *e = dlist_container(GEntry, lru, it.cur);

		if (RelFileLocatorEquals(e->key.locator, *locator))
			drop_entry(e);
	}
	dlist_foreach_modify(it, &fcache_lru)
	{
		FEntry	   *e = dlist_container(FEntry, lru, it.cur);

		if (RelFileLocatorEquals(e->key.granule.locator, *locator))
			fdrop_entry(e);
	}
	dlist_foreach_modify(it, &bcache_lru)
	{
		BEntry	   *e = dlist_container(BEntry, lru, it.cur);

		if (RelFileLocatorEquals(e->key.locator, *locator))
			bdrop_entry(e);
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

bool
sift_f2_locate_cached_read(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	return sift_f2_locate_read(rel, store, tid, sift_f2_gcache_get, loc);
}
