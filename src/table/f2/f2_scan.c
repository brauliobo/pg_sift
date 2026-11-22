/*
 * table/f2/f2_scan.c - reading a sift_table: the granules of its parts as virtual tuples.
 *
 * The parts of the table, and the partitions' pending deletes, are what a scan reads; the rows of a granule that the snapshot sees are one bitmap (f2_mvcc.c), and the rows come out
 * of it in the order of the granule (or backwards). A parallel scan hands units of up to SIFT_TBL_SCAN_UNIT_GRANULES granules of one part to its workers through the unit queue of
 * shared memory; the units are listed once, by the leader, so that the workers read one and the same set of parts. Part pins and decoded granules are those of the shared cache types.
 * A granule of a partition that has column deltas (table/f2/f2_col_delta.h) takes the patches its snapshot sees as its columns are decoded.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/f2_store_schema.h"
#include "core/f2/unit_queue.h"
#include "core/cachebudget.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_gcache.h"
#include "table/f2/f2_slot.h"
#include "table/f2/f2_tbl.h"
#include "core/memtrack.h"

/* ---- the units of a scan ---- */

typedef struct ScanUnit
{
	SiftPartitionEntry entry;
	SiftPartDesc desc;
	uint32		first;
	uint32		n;
} ScanUnit;

typedef struct SiftTblF2Parallel
{
	ParallelTableScanDescData base;
	uint64		nunits;
	SiftUnitQueue queue;
	ScanUnit	units[FLEXIBLE_ARRAY_MEMBER];
} SiftTblF2Parallel;

static ScanUnit *
list_units(Relation rel, SiftF2Store *store, uint64 *nunits)
{
	const SiftPlanSnapshot *v = sift_f2_view(rel, store, CurrentMemoryContext);
	ScanUnit   *units = palloc(sizeof(ScanUnit) * 16);
	uint64		cap = 16,
				n = 0;

	for (int i = 0; i < v->npartitions; i++)
		for (int j = 0; j < v->partlists[i].nparts; j++)
		{
			const SiftPartDesc *d = &v->partlists[i].parts[j];
			SiftF2PartPin *pin = sift_f2_pin_open(rel, store, d, (uint32) v->partitions[i].part_id);
			uint32		ng = pin->part->hdr.ngranules;

			sift_f2_pin_release(pin);
			for (uint32 g = 0; g < ng; g += SIFT_TBL_SCAN_UNIT_GRANULES)
			{
				if (n == cap)
					units = repalloc(units, sizeof(ScanUnit) * (cap *= 2));
				units[n++] = (ScanUnit) {v->partitions[i], *d, g, Min(SIFT_TBL_SCAN_UNIT_GRANULES, ng - g)};
			}
		}
	*nunits = n;
	return units;
}

typedef struct ParImage
{
	Oid			relid;
	uint64		nunits;
	ScanUnit   *units;
} ParImage;

static List *par_images = NIL;

Size
sift_tbl_f2_parallelscan_estimate(Relation rel)
{
	ParImage   *im = MemoryContextAlloc(TopTransactionContext, sizeof(ParImage));
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	im->relid = RelationGetRelid(rel);
	im->units = list_units(rel, sift_f2_store_open(rel), &im->nunits);
	par_images = lappend(par_images, im);
	MemoryContextSwitchTo(old);
	return offsetof(SiftTblF2Parallel, units) + im->nunits * sizeof(ScanUnit);
}

Size
sift_tbl_f2_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	SiftTblF2Parallel *par = (SiftTblF2Parallel *) pscan;
	ParImage   *im = NULL;
	ListCell   *lc;

	foreach(lc, par_images)
		if (((ParImage *) lfirst(lc))->relid == RelationGetRelid(rel))
		{
			im = lfirst(lc);
			par_images = foreach_delete_current(par_images, lc);
			break;
		}
	SIFT_INVARIANT(im != NULL, "sift_table: a parallel scan is initialized without the units its size was taken from");
#if PG_VERSION_NUM >= 180000
	par->base.phs_locator = rel->rd_locator;
#else
	par->base.phs_relid = RelationGetRelid(rel);
#endif
	par->base.phs_syncscan = false;
	par->nunits = im->nunits;
	memcpy(par->units, im->units, im->nunits * sizeof(ScanUnit));
	sift_unit_queue_init(&par->queue, im->nunits);
	return offsetof(SiftTblF2Parallel, units) + im->nunits * sizeof(ScanUnit);
}

void
sift_tbl_f2_parallelscan_reinitialize(Relation rel pg_attribute_unused(), ParallelTableScanDesc pscan)
{
	sift_unit_queue_reset(&((SiftTblF2Parallel *) pscan)->queue);
}

/* ---- the cursor ---- */

typedef struct SiftTblF2Page SiftTblF2Page;

typedef struct SiftTblF2Scan
{
	TableScanDescData base;
	const SiftTableFormatOps *ops;
	SiftF2Store *store;
	MemoryContext cx;
	MemoryContext rowcx;

	ScanUnit   *units;
	uint64		nunits;
	uint64	   *prefix;
	int64		total;
	SiftTblF2Parallel *par;

	int64		span_lo;
	int64		span_hi;
	int64		G;
	int			r;
	bool		started;
	bool		loaded;

	SiftF2GranuleBuf *gb;
	uint64		sel[SIFT_DV_MASK_WORDS];
	SiftTblVis	vis;
	SiftColDeltaScan *delta;
	SiftPendDelIndex *pdel;
	uint64		pdel_part;
	bool		pdel_valid;

	bool		has_range;
	uint64		mintid;
	uint64		maxtid;

	SiftTblF2Page *page;
	BlockNumber sample_blk;
	OffsetNumber sample_max;
} SiftTblF2Scan;

StaticAssertDecl(offsetof(SiftTblF2Scan, ops) == offsetof(SiftTblScanHeader, ops), "a scan starts with SiftTblScanHeader");

static void
scan_set_units(SiftTblF2Scan *s, ScanUnit *units, uint64 n)
{
	s->units = units;
	s->nunits = n;
	s->prefix = MemoryContextAlloc(s->cx, sizeof(uint64) * (n + 1));
	s->prefix[0] = 0;
	for (uint64 i = 0; i < n; i++)
		s->prefix[i + 1] = s->prefix[i] + units[i].n;
	s->total = (int64) s->prefix[n];
}

TableScanDesc
sift_tbl_f2_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
	SiftTblF2Scan *s = palloc0(sizeof(SiftTblF2Scan));
	bool		by_ordinal = (flags & (SO_TYPE_SEQSCAN | SO_TYPE_TIDRANGESCAN | SO_TYPE_ANALYZE)) != 0;

	SIFT_INVARIANT(nkeys == 0, "sift_table: a scan with keys");
	if ((flags & SO_TYPE_ANALYZE) != 0)
		snapshot = GetActiveSnapshot();
	RelationIncrementReferenceCount(rel);
	s->base.rs_rd = rel;
	s->base.rs_snapshot = snapshot;
	s->base.rs_nkeys = nkeys;
	s->base.rs_key = key;
	s->base.rs_flags = flags;
	s->base.rs_parallel = pscan;
	s->ops = &sift_tbl_v2_ops;
	s->store = sift_f2_store_open(rel);
	s->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 scan");
	s->rowcx = AllocSetContextCreate(s->cx, "sift f2 scan row", ALLOCSET_SMALL_SIZES);
	s->pdel_part = 0;
	s->sample_blk = InvalidBlockNumber;
	sift_stage_flush(rel);
	sift_tbl_f2_vis_begin(&s->vis, rel, snapshot);
	s->delta = sift_col_delta_scan_begin(rel, &s->vis, s->cx);
	sift_tbl_f2_predicate_scan(rel, snapshot);
	if (pscan != NULL)
	{
		s->par = (SiftTblF2Parallel *) pscan;
		scan_set_units(s, s->par->units, s->par->nunits);
	}
	else if (by_ordinal)
	{
		MemoryContext old = MemoryContextSwitchTo(s->cx);
		uint64		n;
		ScanUnit   *units = list_units(rel, s->store, &n);

		MemoryContextSwitchTo(old);
		scan_set_units(s, units, n);
	}
	s->span_lo = 0;
	s->span_hi = s->total;
	return &s->base;
}

static void
drop_granule(SiftTblF2Scan *s)
{
	if (s->gb != NULL)
		sift_f2_gbuf_unref(s->gb);
	s->gb = NULL;
	s->loaded = false;
}

void
sift_tbl_f2_scan_end(TableScanDesc scan)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	drop_granule(s);
#if PG_VERSION_NUM >= 180000
	if ((scan->rs_flags & SO_TYPE_BITMAPSCAN) != 0 && !tbm_exhausted(&scan->st.rs_tbmiterator))
		tbm_end_iterate(&scan->st.rs_tbmiterator);
#endif
	if ((scan->rs_flags & SO_TEMP_SNAPSHOT) != 0)
		UnregisterSnapshot(scan->rs_snapshot);
	RelationDecrementReferenceCount(scan->rs_rd);
	MemoryContextDelete(s->cx);
}

void
sift_tbl_f2_scan_rescan(TableScanDesc scan, struct ScanKeyData *key pg_attribute_unused(), bool set_params pg_attribute_unused(), bool allow_strat pg_attribute_unused(),
						bool allow_sync pg_attribute_unused(), bool allow_pagemode pg_attribute_unused())
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	drop_granule(s);
	s->started = false;
	s->span_lo = 0;
	s->span_hi = s->total;
	s->sample_blk = InvalidBlockNumber;
#if PG_VERSION_NUM >= 180000
	if ((scan->rs_flags & SO_TYPE_BITMAPSCAN) != 0 && !tbm_exhausted(&scan->st.rs_tbmiterator))
		tbm_end_iterate(&scan->st.rs_tbmiterator);
#endif
}

static int64
unit_of(const SiftTblF2Scan *s, int64 G)
{
	int64		lo = 0,
				hi = (int64) s->nunits - 1;

	while (lo < hi)
	{
		int64		mid = (lo + hi + 1) / 2;

		if ((int64) s->prefix[mid] <= G)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static void
select_pending(SiftTblF2Scan *s, const SiftPartitionEntry *entry)
{
	if (s->pdel_valid && s->pdel_part == entry->part_id)
		return;
	sift_pdel_idx_free(s->pdel);
	s->pdel = sift_tbl_f2_pending_index(s->base.rs_rd, entry, s->base.rs_snapshot);
	s->pdel_part = entry->part_id;
	s->pdel_valid = true;
}

static void
clip_to_tid_range(SiftTblF2Scan *s)
{
	const uint64 *tids = sift_f2_gbuf_tids(s->gb);

	for (uint16 r = 0; r < s->gb->nrows; r++)
		if (tids[r] < s->mintid || tids[r] > s->maxtid)
			s->sel[r / 64] &= ~(UINT64CONST(1) << (r % 64));
}

/* The granule at ordinal G made current; false when the TID range or the snapshot leaves no row of it */
static bool
load_granule(SiftTblF2Scan *s, int64 G)
{
	const ScanUnit *u = &s->units[unit_of(s, G)];
	uint32		g = u->first + (uint32) (G - (int64) s->prefix[unit_of(s, G)]);
	SiftF2PartPin *pin;
	uint16		n;

	drop_granule(s);
	if (s->has_range && sift_part_tid_excludes(&u->desc, s->mintid, s->maxtid))
		return false;
	pin = sift_f2_pin_open(s->base.rs_rd, s->store, &u->desc, (uint32) u->entry.part_id);
	if (s->has_range && (pin->tidx[g].tid_max < s->mintid || pin->tidx[g].tid_min > s->maxtid))
	{
		sift_f2_pin_release(pin);
		return false;
	}
	s->gb = sift_f2_gbuf_load(s->base.rs_rd, s->store, pin, g);
	sift_f2_pin_release(pin);
	sift_col_delta_scan_attach(s->delta, &u->entry, s->gb);
	select_pending(s, &u->entry);
	n = sift_tbl_f2_vis_granule(&s->vis, s->gb, s->pdel, s->sel);
	if (n > 0 && s->has_range)
		clip_to_tid_range(s);
	if (n == 0)
	{
		drop_granule(s);
		return false;
	}
	s->loaded = true;
	return true;
}

static int
next_selected(const SiftTblF2Scan *s, int step)
{
	int			n = s->gb->nrows,
				p = s->r + step;

	if (step > 0)
	{
		while (p < n)
		{
			uint64		w = s->sel[p / 64] >> (p % 64);

			if (w != 0)
				return p + pg_rightmost_one_pos64(w);
			p = (p / 64 + 1) * 64;
		}
		return -1;
	}
	for (p = Min(p, n - 1); p >= 0;)
	{
		uint64		w = s->sel[p / 64] & (p % 64 == 63 ? ~UINT64CONST(0) : (UINT64CONST(1) << (p % 64 + 1)) - 1);

		if (w != 0)
			return (p / 64) * 64 + pg_leftmost_one_pos64(w);
		p = (p / 64) * 64 - 1;
	}
	return -1;
}

static bool
claim_span(SiftTblF2Scan *s)
{
	uint64		first;
	uint32		count;

	if (s->par == NULL || !sift_unit_queue_next(&s->par->queue, &first, &count))
		return false;
	s->span_lo = (int64) s->prefix[first];
	s->span_hi = (int64) s->prefix[first + count];
	s->G = s->span_lo - 1;
	return true;
}

static bool
next_granule(SiftTblF2Scan *s, int step)
{
	for (;;)
	{
		s->G += step;
		if (s->G < s->span_lo || s->G >= s->span_hi)
		{
			drop_granule(s);
			if (step < 0 || !claim_span(s))
			{
				s->G = step > 0 ? s->span_hi : s->span_lo - 1;
				return false;
			}
			continue;
		}
		CHECK_FOR_INTERRUPTS();
		if (load_granule(s, s->G))
		{
			s->r = step > 0 ? -1 : s->gb->nrows;
			return true;
		}
	}
}

static bool
scan_next(SiftTblF2Scan *s, ScanDirection dir, TupleTableSlot *slot)
{
	int			step = ScanDirectionIsBackward(dir) ? -1 : 1;

	ExecClearTuple(slot);
	MemoryContextReset(s->rowcx);
	if (!s->started)
	{
		s->started = true;
		s->G = step > 0 ? s->span_lo - 1 : s->span_hi;
		if (s->par != NULL)
			s->span_hi = s->span_lo = 0;
	}
	for (;;)
	{
		int			r = s->loaded ? next_selected(s, step) : -1;

		if (r >= 0)
		{
			s->r = r;
			sift_f2_slot_store(slot, s->base.rs_rd, s->gb, (uint16) r, NULL, s->rowcx);
					return true;
		}
		if (!next_granule(s, step))
			return false;
	}
}

bool
sift_tbl_f2_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	return scan_next((SiftTblF2Scan *) scan, direction, slot);
}

void
sift_tbl_f2_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	s->has_range = true;
	s->mintid = sift_tbl_tid_pack(mintid);
	s->maxtid = sift_tbl_tid_pack(maxtid);
}

bool
sift_tbl_f2_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	return scan_next((SiftTblF2Scan *) scan, direction, slot);
}

/* ---- the cursor for ANALYZE ---- */

uint64
sift_tbl_f2_scan_ngranules(TableScanDesc scan)
{
	return (uint64) ((SiftTblF2Scan *) scan)->total;
}

bool
sift_tbl_f2_scan_goto_granule(TableScanDesc scan, uint64 ordinal)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	s->G = (int64) ordinal;
	if (!load_granule(s, s->G))
		return false;
	s->r = -1;
	return true;
}

bool
sift_tbl_f2_scan_next_in_granule(TableScanDesc scan, TupleTableSlot *slot)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;
	int			r;

	MemoryContextReset(s->rowcx);
	if (!s->loaded || (r = next_selected(s, 1)) < 0)
		return false;
	s->r = r;
	sift_f2_slot_store(slot, s->base.rs_rd, s->gb, (uint16) r, NULL, s->rowcx);
	return true;
}

uint32
sift_tbl_f2_scan_granule_rows(TableScanDesc scan)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	return s->gb != NULL ? s->gb->nrows : 0;
}

/* ---- the rows of one virtual block: bitmap scans and TABLESAMPLE ---- */

struct SiftTblF2Page
{
	uint16		n;
	uint16		pos;
	MemoryContext cx;			/* the patches of column deltas of the granules of the page, as the snapshot sees them */
	OffsetNumber off[SIFT_TBL_ROWS_PER_VBLOCK];
	SiftF2GranuleBuf *gb[SIFT_TBL_ROWS_PER_VBLOCK];
	uint16		row[SIFT_TBL_ROWS_PER_VBLOCK];
	const SiftColDeltaGranMap *gm[SIFT_TBL_ROWS_PER_VBLOCK];
};

typedef struct PageLoad
{
	SiftTblF2Scan *s;
	BlockNumber vblock;
	bool		want[SIFT_TBL_ROWS_PER_VBLOCK + 1];
	bool		all;
} PageLoad;

static void
page_drop(SiftTblF2Page *pg)
{
	for (uint16 i = 0; i < pg->n; i++)
		sift_f2_gbuf_unref(pg->gb[i]);
	pg->n = pg->pos = 0;
	MemoryContextReset(pg->cx);
}

/* The first row of the granule whose address is at least addr: the addresses of a lazy granule ascend */
static uint16
first_row_from(const SiftF2GranuleBuf *gb, uint64 addr)
{
	uint16		lo = 0,
				hi = gb->nrows;

	if (!gb->tids_sorted)
		return 0;
	while (lo < hi)
	{
		uint16		mid = (lo + hi) / 2;

		if (gb->tids[mid] < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static bool
page_cb(SiftF2GranuleBuf *gb, const SiftPartitionEntry *entry, void *arg)
{
	PageLoad   *pl = arg;
	SiftTblF2Page *pg = pl->s->page;
	const uint64 *tids = sift_f2_gbuf_tids(gb);
	const SiftColDeltaGranMap *gm = sift_col_delta_scan_map(pl->s->delta, entry, gb);

	select_pending(pl->s, entry);
	if (gm != NULL)
		gm = sift_col_delta_gran_copy(gm, pg->cx);
	for (uint16 r = first_row_from(gb, (uint64) pl->vblock << 16); r < gb->nrows; r++)
	{
		OffsetNumber off = tids[r] & 0xFFFF;

		if ((tids[r] >> 16) != pl->vblock || off == 0 || off > SIFT_TBL_ROWS_PER_VBLOCK || (!pl->all && !pl->want[off]))
		{
			if (gb->tids_sorted && (tids[r] >> 16) > pl->vblock)
				break;
			continue;
		}
		if (!sift_tbl_f2_vis_row(&pl->s->vis, gb, r, pl->s->pdel))
			continue;
		pg->off[pg->n] = off;
		pg->gb[pg->n] = sift_f2_gbuf_ref(gb);
		pg->gm[pg->n] = gm;
		pg->row[pg->n++] = r;
	}
	return false;
}

static void
page_sort(SiftTblF2Page *pg)
{
	for (int i = 1; i < pg->n; i++)
	{
		OffsetNumber off = pg->off[i];
		SiftF2GranuleBuf *gb = pg->gb[i];
		uint16		row = pg->row[i];
		const SiftColDeltaGranMap *gm = pg->gm[i];
		int			j = i - 1;

		for (; j >= 0 && pg->off[j] > off; j--)
		{
			pg->off[j + 1] = pg->off[j];
			pg->gb[j + 1] = pg->gb[j];
			pg->row[j + 1] = pg->row[j];
			pg->gm[j + 1] = pg->gm[j];
		}
		pg->off[j + 1] = off;
		pg->gb[j + 1] = gb;
		pg->row[j + 1] = row;
		pg->gm[j + 1] = gm;
	}
}

static void
page_load(SiftTblF2Scan *s, BlockNumber vblock, const OffsetNumber *offsets, int noffsets)
{
	PageLoad	pl = {.s = s,.vblock = vblock,.all = offsets == NULL};

	if (s->page == NULL)
	{
		s->page = MemoryContextAllocZero(s->cx, sizeof(SiftTblF2Page));
		s->page->cx = AllocSetContextCreate(s->cx, "sift f2 page patches", ALLOCSET_SMALL_SIZES);
	}
	page_drop(s->page);
	for (int i = 0; i < noffsets; i++)
		pl.want[offsets[i]] = true;
	sift_tbl_f2_predicate_point(s->base.rs_rd, (uint64) vblock << 16, s->base.rs_snapshot);
	s->store = sift_f2_store_open(s->base.rs_rd);
	sift_f2_visit_granules_via(s->base.rs_rd, s->store, (uint64) vblock << 16, ((uint64) vblock << 16) | 0xFFFF, sift_f2_gcache_get, page_cb, &pl);
	page_sort(s->page);
}

static bool
page_emit(SiftTblF2Scan *s, TupleTableSlot *slot)
{
	SiftTblF2Page *pg = s->page;

	ExecClearTuple(slot);
	if (pg == NULL || pg->pos >= pg->n)
		return false;
	MemoryContextReset(s->rowcx);
	sift_f2_slot_store(slot, s->base.rs_rd, pg->gb[pg->pos], pg->row[pg->pos], pg->gm[pg->pos], s->rowcx);
	pg->pos++;
	return true;
}

#if PG_VERSION_NUM >= 180000

bool
sift_tbl_f2_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages, uint64 *exact_pages)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	for (;;)
	{
		TBMIterateResult tbmres;

		if (page_emit(s, slot))
			return true;
		if (!tbm_iterate(&scan->st.rs_tbmiterator, &tbmres))
			return false;
		CHECK_FOR_INTERRUPTS();
		if (tbmres.lossy)
		{
			page_load(s, tbmres.blockno, NULL, -1);
			(*lossy_pages)++;
		}
		else
		{
			OffsetNumber offsets[MaxHeapTuplesPerPage];
			int			n = tbm_extract_page_tuple(&tbmres, offsets, lengthof(offsets));

			page_load(s, tbmres.blockno, offsets, n);
			(*exact_pages)++;
		}
		*recheck = tbmres.recheck;
	}
}

#else

bool
sift_tbl_f2_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres)
{
	CHECK_FOR_INTERRUPTS();
	page_load((SiftTblF2Scan *) scan, tbmres->blockno, tbmres->ntuples < 0 ? NULL : tbmres->offsets, tbmres->ntuples);
	return ((SiftTblF2Scan *) scan)->page->n > 0;
}

bool
sift_tbl_f2_scan_bitmap_next_tuple_17(TableScanDesc scan, TBMIterateResult *tbmres pg_attribute_unused(), TupleTableSlot *slot)
{
	return page_emit((SiftTblF2Scan *) scan, slot);
}

#endif

uint64
sift_tbl_f2_nvblocks(Relation rel)
{
	return sift_col_meta_read(rel).next_vblock;
}

static BlockNumber
nvblocks(Relation rel)
{
	return (BlockNumber) Min(sift_tbl_f2_nvblocks(rel), (uint64) MaxBlockNumber);
}

bool
sift_tbl_f2_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber nblocks = nvblocks(scan->rs_rd);

	if (tsm->NextSampleBlock != NULL)
		s->sample_blk = tsm->NextSampleBlock(scanstate, nblocks);
	else
		s->sample_blk = s->sample_blk == InvalidBlockNumber ? 0 : s->sample_blk + 1;
	if (s->sample_blk >= nblocks)
		s->sample_blk = InvalidBlockNumber;
	if (s->sample_blk == InvalidBlockNumber)
		return false;
	page_load(s, s->sample_blk, NULL, -1);
	s->sample_max = s->page->n > 0 ? s->page->off[s->page->n - 1] : 0;
	return true;
}

bool
sift_tbl_f2_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate, TupleTableSlot *slot)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;
	SiftTblF2Page *pg = s->page;
	OffsetNumber off;

	while ((off = scanstate->tsmroutine->NextSampleTuple(scanstate, s->sample_blk, s->sample_max)) != InvalidOffsetNumber)
	{
		CHECK_FOR_INTERRUPTS();
		while (pg->pos < pg->n && pg->off[pg->pos] < off)
			pg->pos++;
		if (pg->pos < pg->n && pg->off[pg->pos] == off)
			return page_emit(s, slot);
	}
	return false;
}

/* ---- fetches by TID ---- */

typedef struct SiftTblF2Fetch
{
	IndexFetchTableData base;
	const SiftTableFormatOps *ops;
	SiftF2Store *store;
	MemoryContext cx;
	MemoryContext rowcx;
	SiftF2GranuleBuf *gb;
	SiftPartitionEntry entry;
	Snapshot	vis_snap;
	SiftTblVis	vis;
	SiftColDeltaScan *delta;
	SiftPendDelIndex *pdel;
	uint64		pdel_part;
	BlockNumber pdel_blk;
	bool		pdel_valid;
} SiftTblF2Fetch;

StaticAssertDecl(offsetof(SiftTblF2Fetch, ops) == offsetof(SiftTblFetchHeader, ops), "a fetch starts with SiftTblFetchHeader");

static SiftTblF2Fetch *
fetch_new(Relation rel, MemoryContext parent)
{
	SiftTblF2Fetch *f = MemoryContextAllocZero(parent, sizeof(SiftTblF2Fetch));

	f->base.rel = rel;
	f->ops = &sift_tbl_v2_ops;
	f->store = sift_f2_store_open(rel);
	f->cx = parent;
	f->delta = sift_col_delta_scan_begin(rel, &f->vis, parent);
	f->rowcx = AllocSetContextCreate(parent, "sift f2 fetch row", ALLOCSET_SMALL_SIZES);
	return f;
}

struct IndexFetchTableData *
sift_tbl_f2_index_fetch_begin(Relation rel SIFT_TAM_FLAGS_PARAM)
{
	return &fetch_new(rel, AllocSetContextCreate(CurrentMemoryContext, "sift f2 fetch", ALLOCSET_SMALL_SIZES))->base;
}

static void
fetch_drop_granule(SiftTblF2Fetch *f)
{
	if (f->gb != NULL)
		sift_f2_gbuf_unref(f->gb);
	f->gb = NULL;
}

void
sift_tbl_f2_index_fetch_reset(struct IndexFetchTableData *data)
{
	fetch_drop_granule((SiftTblF2Fetch *) data);
}

void
sift_tbl_f2_index_fetch_end(struct IndexFetchTableData *data)
{
	SiftTblF2Fetch *f = (SiftTblF2Fetch *) data;

	fetch_drop_granule(f);
	MemoryContextDelete(f->cx);
}

static int
row_in_granule(SiftF2GranuleBuf *gb, uint64 tid)
{
	const SiftTidIndexEntry *t = &gb->pin->tidx[gb->granule_id];

	return tid < t->tid_min || tid > t->tid_max ? -1 : sift_f2_gbuf_row_of(gb, tid);
}

static void
fetch_select_pending(SiftTblF2Fetch *f, Snapshot snapshot)
{
	MemoryContext prev;

	if (f->pdel_valid && f->pdel_part == f->entry.part_id && f->pdel_blk == f->entry.pend_del_blk)
		return;
	prev = MemoryContextSwitchTo(f->cx);
	sift_pdel_idx_free(f->pdel);
	f->pdel = sift_tbl_f2_pending_index(f->base.rel, &f->entry, snapshot);
	f->pdel_part = f->entry.part_id;
	f->pdel_blk = f->entry.pend_del_blk;
	f->pdel_valid = true;
	MemoryContextSwitchTo(prev);
}

static bool
fetch_one(SiftTblF2Fetch *f, uint64 tid, Snapshot snapshot, TupleTableSlot *slot)
{
	Relation	rel = f->base.rel;
	int			row;

	if (f->vis_snap != snapshot)
	{
		fetch_drop_granule(f);
		sift_tbl_f2_vis_begin(&f->vis, rel, snapshot);
		f->vis_snap = snapshot;
		f->pdel_valid = false;
		sift_col_delta_scan_reset(f->delta);
	}
	row = f->gb != NULL ? row_in_granule(f->gb, tid) : -1;
	ExecClearTuple(slot);
	MemoryContextReset(f->rowcx);
	if (row < 0)
	{
		SiftF2Loc	loc;
		bool		waiting;

		fetch_drop_granule(f);
		f->store = sift_f2_store_open(rel);
		if (!sift_tbl_f2_locate_or_wait(rel, f->store, tid, snapshot, &loc, &waiting))
		{
			if (waiting)
				sift_tbl_f2_staged_stub(slot, tid);
			return waiting;
		}
		f->gb = loc.gb;
		f->entry = loc.entry;
		row = loc.row;
	}
	fetch_select_pending(f, snapshot);
	sift_tbl_f2_predicate_point(rel, tid, snapshot);
	if (!sift_tbl_f2_vis_row(&f->vis, f->gb, (uint16) row, f->pdel))
		return false;
	sift_f2_slot_store(slot, rel, f->gb, (uint16) row, sift_col_delta_scan_map(f->delta, &f->entry, f->gb), f->rowcx);
	return true;
}

bool
sift_tbl_f2_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
	*call_again = false;
	if (all_dead != NULL)
		*all_dead = false;
	return fetch_one((SiftTblF2Fetch *) scan, sift_tbl_tid_pack(tid), snapshot, slot);
}

bool
sift_tbl_f2_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift f2 row fetch", ALLOCSET_SMALL_SIZES);
	SiftTblF2Fetch *f = fetch_new(rel, cx);
	bool		found = fetch_one(f, sift_tbl_tid_pack(tid), snapshot, slot);

	if (found)
		ExecMaterializeSlot(slot);
	fetch_drop_granule(f);
	MemoryContextDelete(cx);
	return found;
}

bool
sift_tbl_f2_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return ItemPointerGetBlockNumber(tid) < nvblocks(scan->rs_rd) && ItemPointerGetOffsetNumber(tid) >= 1 && ItemPointerGetOffsetNumber(tid) <= SIFT_TBL_ROWS_PER_VBLOCK;
}
