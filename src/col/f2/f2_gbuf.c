/*
 * col/f2/f2_gbuf.c - the part pins and the decoded granules of the shared Format 2.0 cache (col/f2/f2_gbuf.h); the lookups of rows by address over them are f2_locate.c's.
 *
 * check-memory: allow-allocs every array follows one granule (at most 65,535 rows) or the parts of one manifest, and the cache around them is bounded by its budget
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/delvec_delta.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_gbuf_int.h"
#include "core/cachebudget.h"
#include "core/hash.h"
#include "core/memtrack.h"

/* ---- part pins ---- */

typedef struct PinKey
{
	RelFileLocator locator;
	uint64		part_gen;
	BlockNumber header_blk;		/* a part that a tier evicts keeps its generation and loses its blocks */
	uint32		schema_hash;
} PinKey;

/* The pins nobody holds are on free_lru, the one released longest ago first: the one to drop is its head, and a pin that is held is on no list but that of its relation */
typedef struct PinEntry
{
	PinKey		key;
	Oid			relid;
	SiftF2PartPin *pin;
	dlist_node	lru_node;
	bool		in_free_lru;
	dlist_node	rel_node;
} PinEntry;

#define PIN_CACHE_MAX	256
#define PIN_CACHE_INIT	32
#define PIN_CX_MAX_BLOCK	(16 * 1024)

static struct
{
	HTAB	   *by_key;
	HTAB	   *by_rel;			/* RelList of the pins of each relation */
	MemoryContext cx;
	dlist_head	free_lru;
} pins;

static PinEntry *
pin_victim(void)
{
	return pins.by_key == NULL || dlist_is_empty(&pins.free_lru) ? NULL : dlist_container(PinEntry, lru_node, dlist_head_node(&pins.free_lru));
}

/* The pin of e goes: it is on no list of a walk that is under way */
static void
pin_free(PinEntry *e)
{
	PinKey		key = e->key;

	if (e->in_free_lru)
		dlist_delete(&e->lru_node);
	MemoryContextDelete(e->pin->cx);
	hash_search(pins.by_key, &key, HASH_REMOVE, NULL);
}

static void
pin_drop(PinEntry *e)
{
	rel_list_unlink(pins.by_rel, e->relid, &e->rel_node);
	pin_free(e);
}

static uint64
pin_oldest(void)
{
	PinEntry   *victim = pin_victim();

	return victim != NULL ? victim->pin->lru_tick : UINT64_MAX;
}

static bool
pin_evict_one(void)
{
	PinEntry   *victim = pin_victim();

	if (victim == NULL)
		return false;
	pin_drop(victim);
	return true;
}

/* The pin of an invalidated relation is dropped, or marked to go when its last holder lets go */
static void
pin_invalidate_entry(PinEntry *e)
{
	if (e->pin->refcnt == 0)
		pin_drop(e);
	else
		e->pin->stale = true;
}

void
f2_gbuf_invalidate_pins(Oid relid)
{
	if (pins.by_key == NULL)
		return;
	if (!OidIsValid(relid))
	{
		HASH_SEQ_STATUS seq;
		PinEntry   *e;

		hash_seq_init(&seq, pins.by_key);
		while ((e = hash_seq_search(&seq)) != NULL)
			pin_invalidate_entry(e);
	}
	else
	{
		dlist_head	mine;
		dlist_mutable_iter it;

		rel_list_detach(pins.by_rel, relid, &mine);
		dlist_foreach_modify(it, &mine)
		{
			PinEntry   *e = dlist_container(PinEntry, rel_node, it.cur);

			if (e->pin->refcnt == 0)
				pin_free(e);
			else
			{
				dlist_delete(&e->rel_node);
				rel_list_link(pins.by_rel, relid, &e->rel_node);
				e->pin->stale = true;
			}
		}
	}
}

static void
pin_evict_all(void)
{
	f2_gbuf_invalidate_pins(InvalidOid);
}

static const SiftCacheKind pin_kind = {"f2_part_pins", pin_oldest, pin_evict_one, pin_evict_all};

static void
gbuf_invalidate(uint32 hashvalue)
{
	if (hashvalue == 0)
	{
		f2_gbuf_invalidate_pins(InvalidOid);
		f2_locate_invalidate(InvalidOid);
		return;
	}
	foreach_oid(relid, rel_lists_hits(pins.by_rel, hashvalue))
		f2_gbuf_invalidate_pins(relid);
	f2_locate_invalidate_hits(hashvalue);
}

void
f2_gbuf_register_invalidation(void)
{
	sift_f2_invalidator_register(gbuf_invalidate);
}

static void
pin_cache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(PinKey),.entrysize = sizeof(PinEntry),.hcxt = sift_cache_create_context("sift cache f2 table part pins")};

	pins.cx = ctl.hcxt;
	/* MEM[MEM-IDLE-10]: cachebudget f2 part pins HTAB (32 init entries; grows to PIN_CACHE_MAX) */
	pins.by_key = hash_create("sift cache f2 table part pins", PIN_CACHE_INIT, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	pins.by_rel = rel_lists_create("sift cache f2 table part pins by relation", pins.cx);
	dlist_init(&pins.free_lru);
	sift_cache_register(&pin_kind);
	f2_gbuf_register_invalidation();
}

static void
pin_load_dv(Relation rel, SiftF2PartPin *pin, const SiftPartDesc *d)
{
	uint16	   *rows;

	if (pin->dv_cx != NULL)
		MemoryContextReset(pin->dv_cx);
	pin->dv = NULL;
	pin->dv_gen = d->dv_gen;
	pin->dv_len = 0;
	if (d->dv_blk == InvalidBlockNumber)
		return;
	pin->dv_len = sift_dv_stream_len(rel, d->dv_blk);
	/* MEM[MEM-IDLE-06]: cachebudget f2 part pin lazy dv_cx (0 B if no delvec; 1 KB min on first load) */
	if (pin->dv_cx == NULL)
		pin->dv_cx = AllocSetContextCreateInternal(pin->cx, "sift cache f2 part pin dv", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, SIFT_MEM_MAX_BLOCK);
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
	/* MEM[MEM-IDLE-05]: cachebudget f2 part pin ctx (1 KB init / 16 KB max block so the header image and section arrays are exact-fit chunks; evictable LRU) */
	MemoryContext cx = AllocSetContextCreateInternal(pins.cx, "sift cache f2 part pin", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, PIN_CX_MAX_BLOCK);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftF2PartPin *pin = palloc0(sizeof(SiftF2PartPin));
	uint64		len;
	uint32		ng;

	pin->locator = rel->rd_locator;
	pin->part_gen = d->part_gen;
	pin->part_id = part_id;
	pin->cx = cx;
	pin->part = sift_part_open_desc(rel, d, part_id);
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

SiftF2PartPin *
sift_f2_pin_open(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	PinKey		key = {0};
	PinEntry   *e;
	bool		found;
	bool		built = false;

	if (pins.by_key == NULL)
		pin_cache_init();
	key.locator = rel->rd_locator;
	key.part_gen = d->part_gen;
	key.header_blk = d->header_blk;
	key.schema_hash = store->schema_hash;
	e = hash_search(pins.by_key, &key, HASH_FIND, NULL);
	if (e == NULL)
	{
		SiftF2PartPin *pin;

		if (hash_get_num_entries(pins.by_key) >= PIN_CACHE_MAX)
			pin_evict_one();
		pin = pin_build(rel, store, d, part_id);
		e = hash_search(pins.by_key, &key, HASH_ENTER, &found);
		e->pin = pin;
		e->relid = RelationGetRelid(rel);
		e->in_free_lru = false;
		pin->entry = e;
		rel_list_link(pins.by_rel, e->relid, &e->rel_node);
		built = true;
	}
	else if (e->in_free_lru)
	{
		dlist_delete(&e->lru_node);
		e->in_free_lru = false;
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

/* A pin that nobody holds waits on the list of the pins to drop, unless its relation was invalidated while it was held or the setting keeps nothing */
void
sift_f2_pin_release(SiftF2PartPin *pin)
{
	PinEntry   *e = pin->entry;

	Assert(pin->refcnt > 0);
	if (--pin->refcnt > 0)
		return;
	if (pin->stale || sift_backend_cache_kb == 0)
		pin_drop(e);
	else
	{
		pin->lru_tick = sift_cache_tick();
		dlist_push_tail(&pins.free_lru, &e->lru_node);
		e->in_free_lru = true;
	}
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

	sift_lazy_granule_load(rel, gb->lazy, &tid_chunk, 1);
	(void) sift_f2_gbuf_tids(gb);
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
		gb->tids_sorted = true;
		for (uint16 r = 1; r < gb->nrows && gb->tids_sorted; r++)
			gb->tids_sorted = gb->tids[r - 1] <= gb->tids[r];
	}
	return gb->tids;
}

static uint32
tid_slot(const SiftF2GranuleBuf *gb, uint64 tid)
{
	return (uint32) (sift_fmix64(tid) & gb->tid_ht_mask);
}

/* The table of the rows by address of a granule whose addresses do not ascend: made when the first row is asked for, rows in their order so that of equal addresses the first row is the first found */
static void
tid_table_build(SiftF2GranuleBuf *gb)
{
	MemoryContext old = MemoryContextSwitchTo(gb->cx);

	gb->tid_ht_mask = pg_nextpower2_32(Max(2 * (uint32) gb->nrows, 16)) - 1;
	/* check-memory: allow bounded by SIFT_GRANULE_ROWS */
	gb->tid_ht = palloc0((gb->tid_ht_mask + 1) * sizeof(uint16));
	for (uint32 r = 0; r < gb->nrows; r++)
	{
		uint32		h = tid_slot(gb, gb->tids[r]);

		while (gb->tid_ht[h] != 0)
			h = (h + 1) & gb->tid_ht_mask;
		gb->tid_ht[h] = (uint16) (r + 1);
	}
	MemoryContextSwitchTo(old);
}

/* The first row of the granule at or after which the addresses are not below tid, when they ascend */
static int
tid_lower_bound(const SiftF2GranuleBuf *gb, uint64 tid)
{
	int			lo = 0,
				hi = gb->nrows;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (gb->tids[mid] < tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

int
sift_f2_gbuf_row_of(SiftF2GranuleBuf *gb, uint64 tid)
{
	return sift_f2_gbuf_row_next(gb, tid, -1);
}

int
sift_f2_gbuf_row_next(SiftF2GranuleBuf *gb, uint64 tid, int after)
{
	const uint64 *tids = sift_f2_gbuf_tids(gb);

	if (gb->tids_sorted)
	{
		int			r = after < 0 ? tid_lower_bound(gb, tid) : after + 1;

		return r < gb->nrows && tids[r] == tid ? r : -1;
	}
	if (gb->tid_ht == NULL)
		tid_table_build(gb);
	for (uint32 h = tid_slot(gb, tid); gb->tid_ht[h] != 0; h = (h + 1) & gb->tid_ht_mask)
	{
		int			r = gb->tid_ht[h] - 1;

		if (r > after && tids[r] == tid)
			return r;
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

