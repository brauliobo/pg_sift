/*
 * col/f2/f2_gbuf.c - the part pins and the decoded granules of the shared Format 2.0 cache (col/f2/f2_gbuf.h), and the lookups of rows by address over them.
 *
 * check-memory: allow-allocs every array follows one granule (at most 65,535 rows) or the parts of one manifest, and the cache around them is bounded by its budget
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/pg_class_d.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/f2_cache.h"
#include "col/f2/f2_slab_view.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/l0_slab.h"
#include "col/f2/l0_stage.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

/* ---- part pins ---- */

typedef struct PinKey
{
	RelFileLocator locator;
	uint64		part_gen;
	BlockNumber header_blk;		/* a part that a tier evicts keeps its generation and loses its blocks */
	uint32		schema_hash;
} PinKey;

typedef struct PinEntry
{
	PinKey		key;
	uint32		relhash;		/* sift_f2_rel_hash of the relation, taken when the pin is made */
	SiftF2PartPin *pin;
} PinEntry;

#define PIN_CACHE_MAX	256

static HTAB *pin_cache = NULL;
static MemoryContext pin_root = NULL;

static void gbuf_invalidate(uint32 hashvalue);

static void
pin_drop(PinEntry *e)
{
	PinKey		key = e->key;

	MemoryContextDelete(e->pin->cx);
	hash_search(pin_cache, &key, HASH_REMOVE, NULL);
}

static PinEntry *
pin_victim(void)
{
	HASH_SEQ_STATUS seq;
	PinEntry   *e,
			   *victim = NULL;

	hash_seq_init(&seq, pin_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (e->pin->refcnt == 0 && (victim == NULL || e->pin->lru_tick < victim->pin->lru_tick))
			victim = e;
	return victim;
}

static uint64
pin_oldest(void)
{
	PinEntry   *victim = pin_cache != NULL ? pin_victim() : NULL;

	return victim != NULL ? victim->pin->lru_tick : UINT64_MAX;
}

static bool
pin_evict_one(void)
{
	PinEntry   *victim = pin_cache != NULL ? pin_victim() : NULL;

	if (victim == NULL)
		return false;
	pin_drop(victim);
	return true;
}

/* A pin that a granule or a scan still holds when its relation is invalidated, or when the setting keeps nothing, goes when the last of them lets go */
static void
pin_drop_unwanted(SiftF2PartPin *pin)
{
	HASH_SEQ_STATUS seq;
	PinEntry   *e;

	hash_seq_init(&seq, pin_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (e->pin == pin)
		{
			hash_seq_term(&seq);
			pin_drop(e);
			return;
		}
}

static void
pins_invalidate(uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	PinEntry   *e;

	hash_seq_init(&seq, pin_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (sift_f2_inval_hits(e->relhash, hashvalue))
		{
			if (e->pin->refcnt == 0)
				pin_drop(e);
			else
				e->pin->stale = true;
		}
}

static void
pins_forget(const RelFileLocator *locator)
{
	HASH_SEQ_STATUS seq;
	PinEntry   *e;

	hash_seq_init(&seq, pin_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (RelFileLocatorEquals(e->key.locator, *locator))
		{
			if (e->pin->refcnt == 0)
				pin_drop(e);
			else
			{
				e->pin->uncached = true;
				hash_search(pin_cache, &e->key, HASH_REMOVE, NULL);
			}
		}
}

static void
pin_evict_all(void)
{
	while (pin_evict_one())
		;
}

static const SiftCacheKind pin_kind = {"f2_part_pins", pin_oldest, pin_evict_one, pin_evict_all};

static void
pin_cache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(PinKey),.entrysize = sizeof(PinEntry),.hcxt = sift_cache_create_context("sift cache f2 table part pins")};

	pin_cache = hash_create("sift cache f2 table part pins", PIN_CACHE_MAX, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	pin_root = ctl.hcxt;
	sift_cache_register(&pin_kind);
	sift_f2_invalidator_register(gbuf_invalidate);
}

static void
pin_load_dv(Relation rel, SiftF2PartPin *pin, const SiftPartDesc *d)
{
	uint16	   *rows;

	MemoryContextReset(pin->dv_cx);
	pin->dv = NULL;
	pin->dv_gen = d->dv_gen;
	pin->dv_len = 0;
	if (d->dv_blk == InvalidBlockNumber)
		return;
	pin->dv_len = sift_dv_stream_len(rel, d->dv_blk);
	rows = MemoryContextAlloc(pin->dv_cx, sizeof(uint16) * pin->part->hdr.ngranules);
	for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
		rows[g] = pin->gdir[g].nrows;
	{
		MemoryContext old = MemoryContextSwitchTo(pin->dv_cx);

		pin->dv = sift_dv_read(rel, d, pin->part_id, rows);
		MemoryContextSwitchTo(old);
	}
}

static SiftF2PartPin *
pin_build(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	MemoryContext cx = AllocSetContextCreateInternal(pin_root, "sift cache f2 part pin", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftF2PartPin *pin = palloc0(sizeof(SiftF2PartPin));
	uint64		len;
	uint32		ng;

	pin->locator = rel->rd_locator;
	pin->part_gen = d->part_gen;
	pin->part_id = part_id;
	pin->cx = cx;
	pin->dv_cx = AllocSetContextCreateInternal(cx, "sift cache f2 part pin dv", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	pin->part = sift_f2_slab_desc(d) ? sift_f2_slab_part(rel, store, d, part_id) : sift_part_open_desc(rel, d, part_id);
	pin->zones = palloc0(pin->part->hdr.ncols * sizeof(SiftZone *));
	pin->gdir = sift_part_granule_dir(rel, pin->part);
	pin->tidx = sift_part_read_section(rel, pin->part, SEC_TID_INDEX, &len);
	pin->dicts = sift_part_dicts(rel, pin->part);
	pin->col_map = store->host->build_col_map != NULL ? store->host->build_col_map(store, rel, pin->part, cx) : NULL;
	ng = pin->part->hdr.ngranules;
	pin->tid_min = UINT64_MAX;
	for (uint32 g = 0; g < ng; g++)
	{
		pin->tid_min = Min(pin->tid_min, pin->tidx[g].tid_min);
		pin->tid_max = Max(pin->tid_max, pin->tidx[g].tid_max);
	}
	pin->desc = *d;
	MemoryContextSwitchTo(old);
	pin_load_dv(rel, pin, d);
	return pin;
}

/* The part of a micro-slab is made for the statement from the rows the slab holds now, so no other statement may find it: it lives until it is released, out of the cache */
static SiftF2PartPin *
slab_pin_open(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	SiftF2PartPin *pin = pin_build(rel, store, d, part_id);

	MemoryContextSetParent(pin->cx, TopTransactionContext);
	pin->uncached = true;
	pin->refcnt = 1;
	pin->lru_tick = sift_cache_tick();
	return pin;
}

SiftF2PartPin *
sift_f2_pin_open(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	PinKey		key = {0};
	PinEntry   *e;
	bool		found;
	bool		built = false;

	if (pin_cache == NULL)
		pin_cache_init();
	if (sift_f2_slab_desc(d))
		return slab_pin_open(rel, store, d, part_id);
	key.locator = rel->rd_locator;
	key.part_gen = d->part_gen;
	key.header_blk = d->header_blk;
	key.schema_hash = store->schema_hash;
	e = hash_search(pin_cache, &key, HASH_FIND, NULL);
	if (e == NULL)
	{
		SiftF2PartPin *pin;

		if (hash_get_num_entries(pin_cache) >= PIN_CACHE_MAX)
			pin_evict_one();
		pin = pin_build(rel, store, d, part_id);
		e = hash_search(pin_cache, &key, HASH_ENTER, &found);
		e->pin = pin;
		e->relhash = sift_f2_rel_hash(rel);
		built = true;
	}
	e->pin->stale = false;
	e->pin->refcnt++;
	e->pin->lru_tick = sift_cache_tick();
	if (d->dv_gen != e->pin->dv_gen || (d->dv_blk != InvalidBlockNumber) != (e->pin->dv != NULL) ||
		(sift_dvd_part_takes(d) && sift_dv_stream_len(rel, d->dv_blk) != e->pin->dv_len))
		pin_load_dv(rel, e->pin, d);
	e->pin->desc = *d;
	if (built)
		sift_cache_commit();
	return e->pin;
}

void
sift_f2_pin_release(SiftF2PartPin *pin)
{
	Assert(pin->refcnt > 0);
	if (--pin->refcnt > 0)
		return;
	if (pin->uncached)
		MemoryContextDelete(pin->cx);
	else if (pin->stale || sift_backend_cache_kb == 0)
		pin_drop_unwanted(pin);
}

/* ---- decoded granules ---- */

static void
gbuf_gone(void *arg)
{
	sift_f2_pin_release(((SiftF2GranuleBuf *) arg)->pin);
}

static SiftF2GranuleBuf *
gbuf_make(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g, MemoryContext parent, bool lazy)
{
	MemoryContext cx = sift_mem_allocset_create(parent, lazy ? "sift cache f2 granule" : "sift f2 granule");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftF2GranuleBuf *gb = palloc0(sizeof(SiftF2GranuleBuf));
	MemoryContextCallback *gone = palloc(sizeof(MemoryContextCallback));
	char	   *rec;

	gb->refcnt = 1;
	gb->cx = cx;
	gb->store = lazy ? NULL : store;
	gb->pin = pin;
	pin->refcnt++;
	*gone = (MemoryContextCallback) {gbuf_gone, gb, NULL};
	MemoryContextRegisterResetCallback(cx, gone);
	gb->granule_id = g;
	if (lazy)
	{
		gb->lazy = palloc(sizeof(SiftLazyGranule));
		sift_lazy_granule_open_desc(rel, pin->part, g, &pin->gdir[g], gb->lazy);
		gb->gdesc = gb->lazy->gd;
		rec = gb->lazy->rec;
	}
	else
		rec = sift_part_read_granule(rel, pin->part, g, &gb->gdesc);
	gb->rec = rec;
	gb->view = sift_granule_open_part(rel, pin->part, rec, gb->gdesc.data_len, &gb->gdesc);
	gb->nrows = gb->gdesc.nrows;
	gb->ncols = pin->part->hdr.ncols;
	gb->is_all_frozen = (gb->gdesc.flags & SIFT_GDESC_ALL_FROZEN) != 0;
	gb->is_single_xact = (gb->gdesc.flags & SIFT_GDESC_SINGLE_XACT) != 0;
	if (gb->is_single_xact)
	{
		gb->single_xmin = XidFromFullTransactionId(gb->gdesc.min_xid);
		gb->single_cmin = gb->gdesc.single_cmin;
	}
	else if (!gb->is_all_frozen)
		gb->vers = sift_part_read_vers(rel, pin->part, pin->gdir, g);
	gb->is_all_frozen = gb->is_all_frozen || (!gb->is_single_xact && gb->vers == NULL);
	gb->col_decoded = palloc0(sizeof(bool) * gb->ncols);
	gb->col_i = palloc0(sizeof(int64 *) * gb->ncols);
	gb->col_f = palloc0(sizeof(double *) * gb->ncols);
	gb->col_s = palloc0(sizeof(SiftStrCell *) * gb->ncols);
	gb->col_tag = palloc0(sizeof(uint8 *) * gb->ncols);
	gb->col_isnull = palloc0(sizeof(bool *) * gb->ncols);
	MemoryContextSwitchTo(old);
	return gb;
}

SiftF2GranuleBuf *
sift_f2_gbuf_load(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g)
{
	return gbuf_make(rel, store, pin, g, TopTransactionContext, false);
}

SiftF2GranuleBuf *
sift_f2_gbuf_load_lazy(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g, MemoryContext parent)
{
	SiftF2GranuleBuf *gb = gbuf_make(rel, store, pin, g, parent, true);
	const int	tid_chunk = SIFT_COL_TID_IDX;
	const uint64 *tids;

	sift_lazy_granule_load(rel, gb->lazy, &tid_chunk, 1);
	tids = sift_f2_gbuf_tids(gb);
	gb->tids_sorted = true;
	for (uint16 r = 1; r < gb->nrows && gb->tids_sorted; r++)
		gb->tids_sorted = tids[r - 1] < tids[r];
	return gb;
}

SiftF2GranuleBuf *
sift_f2_gbuf_ref(SiftF2GranuleBuf *gb)
{
	gb->refcnt++;
	return gb;
}

void
sift_f2_gbuf_unref(SiftF2GranuleBuf *gb)
{
	if (--gb->refcnt > 0)
		return;
	MemoryContextDelete(gb->cx);
}

const uint64 *
sift_f2_gbuf_tids(SiftF2GranuleBuf *gb)
{
	if (gb->tids == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(gb->cx);

		gb->tids = palloc(sizeof(uint64) * gb->nrows);
		sift_granule_tids(&gb->view, gb->tids);
		MemoryContextSwitchTo(old);
	}
	return gb->tids;
}

int
sift_f2_gbuf_row_of(SiftF2GranuleBuf *gb, uint64 tid)
{
	const uint64 *tids = sift_f2_gbuf_tids(gb);
	int			lo = 0,
				hi = (int) gb->nrows - 1;

	if (!gb->tids_sorted)
	{
		for (int r = 0; r < gb->nrows; r++)
			if (tids[r] == tid)
				return r;
		return -1;
	}
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (tids[mid] == tid)
			return mid;
		if (tids[mid] < tid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

static void
decode_column(SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, bool wide, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);

	gb->col_isnull[phys] = palloc(sizeof(bool) * gb->nrows);
	if (kind == SIFT_CELL_INT)
	{
		gb->col_i[phys] = palloc(sizeof(int64) * gb->nrows);
		sift_granule_ints(&gb->view, phys, gb->col_i[phys], gb->col_isnull[phys]);
		if (wide)
		{
			gb->col_tag[phys] = palloc(gb->nrows);
			sift_granule_tags(&gb->view, phys, gb->col_tag[phys]);
		}
	}
	else if (kind == SIFT_CELL_F8)
	{
		gb->col_f[phys] = palloc(sizeof(double) * gb->nrows);
		sift_granule_f8(&gb->view, phys, gb->col_f[phys], gb->col_isnull[phys]);
	}
	else
	{
		SiftStrCell *cells = palloc(sizeof(SiftStrCell) * gb->nrows);

		sift_granule_strs(&gb->view, phys, cells);
		gb->col_s[phys] = cells;
		for (uint16 r = 0; r < gb->nrows; r++)
			gb->col_isnull[phys][r] = cells[r].isnull;
		if (wide)
		{
			gb->col_tag[phys] = palloc(gb->nrows);
			sift_granule_tags(&gb->view, phys, gb->col_tag[phys]);
		}
	}
	gb->col_decoded[phys] = true;
	MemoryContextSwitchTo(old);
}

void
sift_f2_gbuf_decode(SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, bool wide)
{
	decode_column(gb, phys, kind, wide, gb->cx);
	if (gb->patch != NULL)
		gb->patch(gb, phys);
}

void
sift_f2_gbuf_column(Relation rel, SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, bool wide)
{
	const int	chunk = phys + 1;

	if (gb->lazy == NULL)
	{
		sift_f2_gbuf_decode(gb, phys, kind, wide);
		return;
	}
	sift_lazy_granule_load(rel, gb->lazy, &chunk, 1);
	decode_column(gb, phys, kind, wide, gb->cx);
}

Size
sift_f2_gbuf_bytes(const SiftF2GranuleBuf *gb)
{
	return MemoryContextMemAllocated(gb->cx, true);
}

/* ---- the parts by address ---- */

/* The envelope of the addresses of a part (SiftPartDesc.tid_min, tid_max) and the greatest tid_max of the slots up to this one: a lookup walks back from the last slot that starts at or before the address, and stops where that is below it */
typedef struct TidSlot
{
	uint64		tid_min;
	uint64		tid_max;
	uint64		prefix_max;
	int32		partition;
	int32		part;
} TidSlot;

/* The parts of the manifest of one version, by address; made when a lookup meets the version first and kept (one for each relation file of the backend) until another version comes */
typedef struct TidIndex
{
	RelFileLocator locator;		/* first: the hash key */
	uint32		relhash;		/* sift_f2_rel_hash of the relation, taken when the index is made */
	uint64		manifest_version;
	uint64		tick;			/* sift_cache_tick() of its last use */
	MemoryContext cx;			/* holds slots and loose */
	int			n;
	TidSlot    *slots;			/* the parts with an envelope, ascending by tid_min */
	int			nloose;
	TidSlot    *loose;			/* the parts without one: every address may be in them */
} TidIndex;

#define TID_INDEX_MAX	64

static HTAB *tid_indexes = NULL;
static MemoryContext tid_cx = NULL;
static MemoryContext tid_retired = NULL;	/* the indexes replaced or forgotten while a lookup walks one: deleted when the last lookup is done */
static int	tid_visits = 0;		/* lookups that walk an index now: it is not evicted under them */

static void
tid_index_free(TidIndex *ix)
{
	if (tid_visits > 0)
		MemoryContextSetParent(ix->cx, tid_retired);
	else
		MemoryContextDelete(ix->cx);
}

static int
slot_cmp(const void *a, const void *b)
{
	const TidSlot *x = a,
			   *y = b;

	return x->tid_min != y->tid_min ? (x->tid_min > y->tid_min) - (x->tid_min < y->tid_min) : (x->tid_max > y->tid_max) - (x->tid_max < y->tid_max);
}

static TidIndex *
tid_index_victim(void)
{
	HASH_SEQ_STATUS seq;
	TidIndex   *ix,
			   *victim = NULL;

	if (tid_indexes == NULL || tid_visits > 0)
		return NULL;
	hash_seq_init(&seq, tid_indexes);
	while ((ix = hash_seq_search(&seq)) != NULL)
		if (victim == NULL || ix->tick < victim->tick)
			victim = ix;
	return victim;
}

static uint64
tid_index_oldest(void)
{
	TidIndex   *victim = tid_index_victim();

	return victim != NULL ? victim->tick : UINT64_MAX;
}

static void
tid_index_drop(TidIndex *ix)
{
	RelFileLocator locator = ix->locator;

	tid_index_free(ix);
	hash_search(tid_indexes, &locator, HASH_REMOVE, NULL);
}

static bool
tid_index_evict_one(void)
{
	TidIndex   *victim = tid_index_victim();

	if (victim == NULL)
		return false;
	tid_index_drop(victim);
	return true;
}

static void
tid_indexes_invalidate(uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	TidIndex   *ix;

	if (tid_visits > 0)
		return;
	hash_seq_init(&seq, tid_indexes);
	while ((ix = hash_seq_search(&seq)) != NULL)
		if (sift_f2_inval_hits(ix->relhash, hashvalue))
			tid_index_drop(ix);
}

static void
gbuf_invalidate(uint32 hashvalue)
{
	if (pin_cache != NULL)
		pins_invalidate(hashvalue);
	if (tid_indexes != NULL)
		tid_indexes_invalidate(hashvalue);
}

void
sift_f2_gbuf_forget(const RelFileLocator *locator)
{
	TidIndex   *ix = tid_indexes != NULL ? hash_search(tid_indexes, locator, HASH_FIND, NULL) : NULL;

	if (pin_cache != NULL)
		pins_forget(locator);
	if (ix == NULL)
		return;
	if (tid_visits > 0)
		ix->manifest_version = UINT64_MAX;
	else
		tid_index_drop(ix);
}

static void
tid_index_evict_all(void)
{
	while (tid_index_evict_one())
		;
}

static const SiftCacheKind tid_index_kind = {"f2_tid_indexes", tid_index_oldest, tid_index_evict_one, tid_index_evict_all};

static TidIndex
tid_index_build(const SiftPlanSnapshot *v, MemoryContext parent)
{
	TidIndex	ix = {.locator = v->locator,.manifest_version = v->manifest_version};
	int			total = 0;

	for (int i = 0; i < v->npartitions; i++)
		total += v->partlists[i].nparts;
	ix.cx = AllocSetContextCreateInternal(parent, "sift cache f2 tid index", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
	ix.slots = MemoryContextAlloc(ix.cx, sizeof(TidSlot) * Max(total, 1));
	ix.loose = MemoryContextAlloc(ix.cx, sizeof(TidSlot) * Max(total, 1));
	for (int i = 0; i < v->npartitions; i++)
		for (int j = 0; j < v->partlists[i].nparts; j++)
		{
			const SiftPartDesc *d = &v->partlists[i].parts[j];
			TidSlot		slot = {.tid_min = d->tid_min,.tid_max = d->tid_max,.partition = i,.part = j};

			if (d->tid_max != 0)
				ix.slots[ix.n++] = slot;
			else
				ix.loose[ix.nloose++] = slot;
		}
	qsort(ix.slots, ix.n, sizeof(TidSlot), slot_cmp);
	for (int k = 0; k < ix.n; k++)
		ix.slots[k].prefix_max = Max(k > 0 ? ix.slots[k - 1].prefix_max : 0, ix.slots[k].tid_max);
	return ix;
}

/* The index of the version of the manifest the view has, kept for the next lookup; one of a view that lists micro-slabs is made for the lookup, in mcxt */
static const TidIndex *
tid_index_of(Relation rel, const SiftPlanSnapshot *v, MemoryContext mcxt)
{
	TidIndex   *e;
	TidIndex	built;

	if (tid_indexes == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(RelFileLocator),.entrysize = sizeof(TidIndex),.hcxt = sift_cache_create_context("sift cache f2 tid indexes")};

		tid_cx = ctl.hcxt;
		tid_retired = AllocSetContextCreate(tid_cx, "sift cache f2 tid indexes dropped", ALLOCSET_SMALL_SIZES);
		tid_indexes = hash_create("sift cache f2 tid indexes", TID_INDEX_MAX, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		sift_cache_register(&tid_index_kind);
		sift_f2_invalidator_register(gbuf_invalidate);
	}
	if (v->slabs_in_memory)
	{
		TidIndex   *own = MemoryContextAlloc(mcxt, sizeof(TidIndex));

		*own = tid_index_build(v, mcxt);
		return own;
	}
	e = hash_search(tid_indexes, &v->locator, HASH_FIND, NULL);
	if (e != NULL && e->manifest_version == v->manifest_version)
	{
		e->tick = sift_cache_tick();
		return e;
	}
	built = tid_index_build(v, tid_cx);
	built.relhash = sift_f2_rel_hash(rel);
	built.tick = sift_cache_tick();
	if (e != NULL)
		tid_index_free(e);
	else if (hash_get_num_entries(tid_indexes) >= TID_INDEX_MAX)
		tid_index_evict_one();
	e = hash_search(tid_indexes, &v->locator, HASH_ENTER, NULL);
	*e = built;
	return e;
}

typedef bool (*TidSlotVisit) (const TidSlot *slot, void *arg);

/* Calls visit for the parts that may hold an address of [lo, hi]: by the envelope of each, in O(log parts) for the parts that do not overlap, and the parts without an envelope; visit returns true to stop */
static void
tid_index_visit(const TidIndex *ix, uint64 lo, uint64 hi, TidSlotVisit visit, void *arg)
{
	int			first = 0,
				last = ix->n;

	while (first < last)
	{
		int			mid = first + (last - first) / 2;

		if (ix->slots[mid].tid_min <= hi)
			first = mid + 1;
		else
			last = mid;
	}
	for (int k = first - 1; k >= 0 && ix->slots[k].prefix_max >= lo; k--)
		if (ix->slots[k].tid_max >= lo && visit(&ix->slots[k], arg))
			return;
	for (int k = 0; k < ix->nloose; k++)
		if (visit(&ix->loose[k], arg))
			return;
}

/* A visit reads the index of the relation while it opens pins, which may ask the budget for room: the index is not dropped until the visit ends */
static void
tid_visit(Relation rel, const SiftPlanSnapshot *v, MemoryContext mcxt, uint64 lo, uint64 hi, TidSlotVisit visit, void *arg)
{
	const TidIndex *ix = tid_index_of(rel, v, mcxt);

	tid_visits++;
	PG_TRY();
	{
		tid_index_visit(ix, lo, hi, visit, arg);
	}
	PG_FINALLY();
	{
		if (--tid_visits == 0)
			MemoryContextReset(tid_retired);
	}
	PG_END_TRY();
}

/* ---- the manifest ---- */

/* The slabs of the partitions written out as parts; a standby writes nothing, so its view lists them as parts made in memory */
static const SiftPlanSnapshot *
slabs_flushed(Relation rel, SiftF2Store *store, const SiftPlanSnapshot *snap, MemoryContext mcxt)
{
	bool		flushed = false;

	for (int i = 0; i < snap->npartitions; i++)
		if (snap->partitions[i].l0_slab_blk != InvalidBlockNumber)
		{
			sift_l0_slab_flush(rel, snap->partitions[i].part_id, snap->partitions[i].key_lo, &store->spec);
			flushed = true;
		}
	return flushed ? sift_plan_cache_get(rel, mcxt) : snap;
}

/* The view and what writing the slabs takes are mcxt's: a statement that locates a row for each of its rows does not keep them in its executor context */
static const SiftPlanSnapshot *
view_slabs_flushed(Relation rel, SiftF2Store *store, MemoryContext mcxt)
{
	MemoryContext caller = MemoryContextSwitchTo(mcxt);
	const SiftPlanSnapshot *snap = sift_plan_cache_get(rel, mcxt);

	snap = RecoveryInProgress() ? sift_f2_slab_view(rel, store, snap, mcxt) : slabs_flushed(rel, store, snap, mcxt);
	MemoryContextSwitchTo(caller);
	return snap;
}

const SiftPlanSnapshot *
sift_f2_view(Relation rel, SiftF2Store *store, MemoryContext mcxt)
{
	if (sift_stage_has_pending(rel))
		sift_stage_flush(rel);
	return view_slabs_flushed(rel, store, mcxt);
}

typedef struct VisitPart
{
	Relation	rel;
	SiftF2Store *store;
	const SiftPlanSnapshot *view;
	uint64		lo;
	uint64		hi;
	bool		one_block;
	SiftF2GranuleLoader load;
	SiftF2GranuleVisit cb;
	void	   *arg;
} VisitPart;

static bool
visit_part(const TidSlot *slot, void *arg)
{
	VisitPart  *vp = arg;
	const SiftPartitionEntry *entry = &vp->view->partitions[slot->partition];
	SiftF2PartPin *pin = sift_f2_pin_open(vp->rel, vp->store, &vp->view->partlists[slot->partition].parts[slot->part], (uint32) entry->part_id);
	bool		stop = false;

	PG_TRY();
	{
		for (uint32 g = 0; g < pin->part->hdr.ngranules && !stop && pin->tid_max >= vp->lo && pin->tid_min <= vp->hi; g++)
		{
			const SiftTidIndexEntry *t = &pin->tidx[g];
			SiftF2GranuleBuf *gb;

			if (t->tid_max < vp->lo || t->tid_min > vp->hi || (vp->one_block && (t->blk_bloom & sift_tid_block_bit(vp->lo)) == 0))
				continue;
			gb = vp->load(vp->rel, vp->store, pin, g);
			stop = vp->cb(gb, entry, vp->arg);
			sift_f2_gbuf_unref(gb);
		}
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	return stop;
}

static bool
has_slab(const SiftPlanSnapshot *snap)
{
	for (int i = 0; i < snap->npartitions; i++)
		if (snap->partitions[i].l0_slab_blk != InvalidBlockNumber)
			return true;
	return false;
}

/* Visits the granules that may hold the addresses lo..hi, each made by load; false when it left the micro-slabs of the partitions unflushed (flush false) and some partition has one */
static bool
visit_view(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg, bool flush)
{
	MemoryContext pins = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 visit");
	const SiftPlanSnapshot *v;
	bool		slabs_left;
	VisitPart	vp;

	v = flush ? view_slabs_flushed(rel, store, pins) : sift_plan_cache_get(rel, pins);
	slabs_left = !flush && has_slab(v);
	vp = (VisitPart) {rel, store, v, lo, hi, (lo >> 16) == (hi >> 16), load, cb, arg};
	tid_visit(rel, v, pins, lo, hi, visit_part, &vp);
	MemoryContextDelete(pins);
	return slabs_left;
}

static bool
visit_granules(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg, bool flush)
{
	if (sift_stage_holds_in(rel, lo, hi))
		sift_stage_flush(rel);
	return visit_view(rel, store, lo, hi, load, cb, arg, flush);
}

/* The parts of the partitions as they are published, the stage of the transaction left alone: its rows are the caller's to read */
static void
visit_settled(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleVisit cb, void *arg)
{
	(void) visit_view(rel, store, lo, hi, sift_f2_gbuf_load, cb, arg, true);
}

void
sift_f2_visit_granules_via(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg)
{
	(void) visit_granules(rel, store, lo, hi, load, cb, arg, true);
}

void
sift_f2_visit_granules(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleVisit cb, void *arg)
{
	sift_f2_visit_granules_via(rel, store, lo, hi, sift_f2_gbuf_load, cb, arg);
}

typedef struct LocateArg
{
	uint64		tid;
	SiftF2Loc *loc;
	bool		found;
	bool		live_only;
} LocateArg;

/* An index has no versions of its own: after a VACUUM the heap gives an address to a later row, so a part may hold a row of the address that is dead already and a live one after it */
static bool
row_removed(const SiftF2GranuleBuf *gb, int row)
{
	const SiftDvGran *dg = gb->pin->dv != NULL ? sift_dv_granule(gb->pin->dv, gb->granule_id) : NULL;
	bool		dead = false;

	if (dg != NULL)
		(void) sift_dv_find(dg, (uint16) row, &dead);
	return dead;
}

static int
live_row_of(SiftF2GranuleBuf *gb, uint64 tid)
{
	const uint64 *tids = sift_f2_gbuf_tids(gb);
	int			row = sift_f2_gbuf_row_of(gb, tid);

	if (row < 0 || !row_removed(gb, row))
		return row;
	for (int r = 0; r < gb->nrows; r++)
		if (tids[r] == tid && !row_removed(gb, r))
			return r;
	return -1;
}

static bool
locate_cb(SiftF2GranuleBuf *gb, const SiftPartitionEntry *entry, void *arg)
{
	LocateArg  *a = arg;
	int			row = a->live_only ? live_row_of(gb, a->tid) : sift_f2_gbuf_row_of(gb, a->tid);

	if (row < 0)
		return false;
	a->loc->entry = *entry;
	a->loc->gb = sift_f2_gbuf_ref(gb);
	a->loc->row = (uint16) row;
	a->found = true;
	return true;
}

bool
sift_f2_locate_settled(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	LocateArg	a = {.tid = tid,.loc = loc,.live_only = rel->rd_rel->relkind == RELKIND_INDEX};

	visit_settled(rel, store, tid, tid, locate_cb, &a);
	return a.found;
}

bool
sift_f2_locate_published(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	MemoryContext pins = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 visit");
	LocateArg	a = {.tid = tid,.loc = loc,.live_only = rel->rd_rel->relkind == RELKIND_INDEX};
	const SiftPlanSnapshot *v = sift_plan_cache_get(rel, pins);
	VisitPart	vp = {rel, store, v, tid, tid, true, sift_f2_gbuf_load, locate_cb, &a};

	tid_visit(rel, v, pins, tid, tid, visit_part, &vp);
	MemoryContextDelete(pins);
	return a.found;
}

/* The granule of an address, each granule made by load: the parts are searched as they are, and the micro-slabs are flushed into parts only when the address is in none (it is a row of a slab), so that a point lookup of a row in a part writes nothing */
bool
sift_f2_locate_via(Relation rel, SiftF2Store *store, uint64 tid, SiftF2GranuleLoader load, SiftF2Loc *loc)
{
	LocateArg	a = {.tid = tid,.loc = loc,.live_only = rel->rd_rel->relkind == RELKIND_INDEX};

	if (visit_granules(rel, store, tid, tid, load, locate_cb, &a, false) && !a.found)
		(void) visit_granules(rel, store, tid, tid, load, locate_cb, &a, true);
	return a.found;
}
