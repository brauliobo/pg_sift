/*
 * table/f2/scan.c - reading a sift_table: the granules of its parts as virtual tuples.
 *
 * The parts of the table, and the partitions' pending deletes, are what a scan reads; the rows of a granule that the snapshot sees are one bitmap (f2_mvcc.c), and the rows come out
 * of it in the order of the granule (or backwards). A parallel scan lists units of up to SIFT_TBL_SCAN_UNIT_GRANULES granules of one part, once, in the leader, so that the workers read one
 * and the same set of parts, and hands the granules of the units to its workers as the guided morsels of the queue of shared memory that follows them (core/par/morsel.h): a table of a few units is shared by all of them. Part pins and decoded granules are those of the shared cache types.
 * A granule of a partition that has column deltas (table/f2/col_delta/col_delta.h) takes the patches its snapshot sees as its columns are decoded.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "optimizer/cost.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "col/f2/slab_view.h"
#include "col/f2/store/schema.h"
#include "core/cachebudget.h"
#include "core/maint/pacer.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/gcache.h"
#include "table/f2/slot.h"
#include "table/f2/tbl.h"
#include "core/memtrack.h"
#include "core/par/morsel.h"

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
	ScanUnit	units[FLEXIBLE_ARRAY_MEMBER];
	/* then, at the next SIFT_PAR_SHM_ALIGN, the SiftMorselQueue of the granules of the units */
} SiftTblF2Parallel;

static SiftMorselQueue *
par_queue(SiftTblF2Parallel *par)
{
	return (SiftMorselQueue *) TYPEALIGN(SIFT_PAR_SHM_ALIGN, par->units + par->nunits);
}

static void
par_queue_init(SiftTblF2Parallel *par)
{
	/* check-memory: allow the units are those the leader listed in shared memory, one 4-byte count each */
	uint32	   *granules = palloc(Max(par->nunits, 1) * sizeof(uint32));

	for (uint64 i = 0; i < par->nunits; i++)
		granules[i] = par->units[i].n;
	sift_morsel_init(par_queue(par), (uint32) par->nunits, granules, max_parallel_workers_per_gather + 1);
	pfree(granules);
}

/* The parts of a view in the order of a scan, taken one at a time: the units of a part are known once its header is read */
typedef struct UnitCursor
{
	const SiftPlanSnapshot *view;
	int			part;
	int			next;
} UnitCursor;

typedef struct UnitList
{
	ScanUnit   *units;
	uint64		n;
	uint64		cap;
} UnitList;

/* Appends the units of the next part the range does not leave out (none with a part its address envelope excludes: that part is not even opened); false when the view has no part left */
static bool
cursor_take(Relation rel, SiftF2Store *store, UnitCursor *c, UnitList *out, const uint64 *tid_range)
{
	const SiftPlanSnapshot *v = c->view;
	const SiftPartDesc *d;
	SiftF2PartPin *pin;
	uint32		ng;

	while (c->part < v->npartitions && c->next >= v->partlists[c->part].nparts)
	{
		c->part++;
		c->next = 0;
	}
	if (c->part >= v->npartitions)
		return false;
	d = &v->partlists[c->part].parts[c->next++];
	if (tid_range != NULL && sift_part_tid_excludes(d, tid_range[0], tid_range[1]))
		return true;
	pin = sift_f2_pin_open_header(rel, store, d, (uint32) v->partitions[c->part].part_id);
	ng = pin->part->hdr.ngranules;
	sift_f2_pin_release(pin);
	for (uint32 g = 0; g < ng; g += SIFT_TBL_SCAN_UNIT_GRANULES)
	{
		if (out->n == out->cap)
			out->units = repalloc(out->units, sizeof(ScanUnit) * (out->cap *= 2));
		out->units[out->n++] = (ScanUnit) {v->partitions[c->part], *d, g, Min(SIFT_TBL_SCAN_UNIT_GRANULES, ng - g)};
	}
	return true;
}

static ScanUnit *
list_units(Relation rel, SiftF2Store *store, uint64 *nunits)
{
	UnitCursor	c = {.view = sift_f2_parallel_view(rel, store, CurrentMemoryContext)};
	UnitList	l = {.units = palloc(sizeof(ScanUnit) * 16),.cap = 16};

	while (cursor_take(rel, store, &c, &l, NULL))
		;
	*nunits = l.n;
	return l.units;
}

typedef struct ParImage
{
	Oid			relid;
	uint64		nunits;
	ScanUnit   *units;
	MemoryContext cx;			/* the image and everything its listing took: deleted when the scan is set up */
} ParImage;

static List *par_images = NIL;
static bool par_hooked;

static void
par_xact_end(XactEvent event pg_attribute_unused(), void *arg pg_attribute_unused())
{
	par_images = NIL;
}

static void
par_subxact_end(SubXactEvent event, SubTransactionId mySubid pg_attribute_unused(), SubTransactionId parentSubid pg_attribute_unused(), void *arg pg_attribute_unused())
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		par_images = NIL;
}

Size
sift_tbl_f2_parallelscan_estimate(Relation rel)
{
	MemoryContext cx = sift_mem_allocset_create(TopTransactionContext, "sift table parallel scan units");
	MemoryContext old;
	ParImage   *im;

	sift_stage_flush_with_deletes(rel);
	old = MemoryContextSwitchTo(cx);
	im = palloc(sizeof(ParImage));
	im->relid = RelationGetRelid(rel);
	im->cx = cx;
	im->units = list_units(rel, sift_f2_store_open(rel), &im->nunits);
	MemoryContextSwitchTo(TopTransactionContext);
	if (!par_hooked)
	{
		RegisterXactCallback(par_xact_end, NULL);
		RegisterSubXactCallback(par_subxact_end, NULL);
		par_hooked = true;
	}
	par_images = lappend(par_images, im);
	MemoryContextSwitchTo(old);
	return offsetof(SiftTblF2Parallel, units) + im->nunits * sizeof(ScanUnit) + SIFT_PAR_SHM_ALIGN + sift_morsel_size((uint32) im->nunits);
}

Size
sift_tbl_f2_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	SiftTblF2Parallel *par = (SiftTblF2Parallel *) pscan;
	ParImage   *im = NULL;
	ListCell   *lc;
	Size		len;

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
	par_queue_init(par);
	len = offsetof(SiftTblF2Parallel, units) + im->nunits * sizeof(ScanUnit) + SIFT_PAR_SHM_ALIGN + sift_morsel_size((uint32) im->nunits);
	MemoryContextDelete(im->cx);
	return len;
}

void
sift_tbl_f2_parallelscan_reinitialize(Relation rel pg_attribute_unused(), ParallelTableScanDesc pscan)
{
	par_queue_init((SiftTblF2Parallel *) pscan);
}

/* ---- the cursor ---- */

typedef struct SiftTblF2Page SiftTblF2Page;

static void page_drop(SiftTblF2Page *pg);

/* MEM[MEM-TBL-04]: struct SiftTblF2Scan (1816 B packed; per-scan ctx + rowcx) */
typedef struct SiftTblF2Scan
{
	TableScanDescData base;
	const SiftTableFormatOps *ops;
	SiftF2Store *store;
	MemoryContext cx;
	MemoryContext rowcx;

	ScanUnit   *units;
	uint64		nunits;
	uint64		units_cap;
	uint64	   *prefix;
	int64		total;
	UnitCursor	cursor;
	bool		units_complete;	/* the cursor has no part left: nunits and total are the scan's */
	SiftTblF2Parallel *par;

	int64		span_lo;
	int64		span_hi;
	int64		G;
	int			r;
	bool		started;
	bool		loaded;

	SiftF2GranuleBuf *gb;
	uint64		pin_part_id;
	SiftF2PartPin *pin;			/* the part of gb, kept from one of its granules to the next: opening it again asks the delete vector of the part for its length */
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

	uint64		an_next;		/* ANALYZE: the next granule of the block of the sample, and the end of them */
	uint64		an_end;
	uint16		hidden;			/* the rows of the granule made current that no row of the snapshot sees */

	bool		fg_sampled;		/* the pacer of the maintenance workers times this scan: it ends with the scan */
	TimestampTz fg_t0;
} SiftTblF2Scan;

StaticAssertDecl(offsetof(SiftTblF2Scan, ops) == offsetof(SiftTblScanHeader, ops), "a scan starts with SiftTblScanHeader");

static void
scan_set_units(SiftTblF2Scan *s, ScanUnit *units, uint64 n)
{
	s->units = units;
	s->nunits = n;
	s->units_cap = n;
	s->prefix = MemoryContextAlloc(s->cx, sizeof(uint64) * (n + 1));
	s->prefix[0] = 0;
	for (uint64 i = 0; i < n; i++)
		s->prefix[i + 1] = s->prefix[i] + units[i].n;
	s->total = (int64) s->prefix[n];
	s->units_complete = true;
}

/* The units of an ordinal scan start with none: a part is opened when the scan reaches it */
static void
scan_units_restart(SiftTblF2Scan *s)
{
	s->nunits = 0;
	s->total = 0;
	s->units_complete = false;
	s->cursor.part = 0;
	s->cursor.next = 0;
	s->prefix[0] = 0;
}

/* The units of the next part that has any; false when the scan has no part left */
static bool
scan_open_next_part(SiftTblF2Scan *s)
{
	uint64		range[2] = {s->mintid, s->maxtid};
	UnitList	l = {s->units, s->nunits, s->units_cap};
	uint64		before = s->nunits;

	while (!s->units_complete && l.n == before)
		if (!cursor_take(s->base.rs_rd, s->store, &s->cursor, &l, s->has_range ? range : NULL))
			s->units_complete = true;
	s->units = l.units;
	s->nunits = l.n;
	if (l.cap > s->units_cap)
	{
		s->units_cap = l.cap;
		s->prefix = repalloc(s->prefix, sizeof(uint64) * (l.cap + 1));
	}
	for (uint64 i = before; i < l.n; i++)
		s->prefix[i + 1] = s->prefix[i] + s->units[i].n;
	s->total = (int64) s->prefix[l.n];
	return l.n > before;
}

static void
scan_complete_units(SiftTblF2Scan *s)
{
	while (!s->units_complete)
		(void) scan_open_next_part(s);
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
	if ((flags & SO_TYPE_SEQSCAN) != 0)
		pgstat_count_heap_scan(rel);
	s->base.rs_rd = rel;
	s->base.rs_snapshot = snapshot;
	s->base.rs_nkeys = nkeys;
	s->base.rs_key = key;
	s->base.rs_flags = flags;
	s->base.rs_parallel = pscan;
	s->ops = &sift_tbl_v2_ops;
	s->store = sift_f2_store_open(rel);
	/* MEM[MEM-TBL-10]: query-scoped f2 scan ctx (256 KB max) + rowcx (1 KB small; reset/row) */
	s->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 scan");
	s->rowcx = AllocSetContextCreate(s->cx, "sift f2 scan row", ALLOCSET_SMALL_SIZES);
	s->pdel_part = 0;
	s->sample_blk = InvalidBlockNumber;
	sift_stage_flush(rel);
	sift_tbl_f2_vis_begin(&s->vis, rel, snapshot);
	s->delta = sift_col_delta_scan_begin(rel, &s->vis, s->cx);
	sift_tbl_f2_predicate_scan(rel, snapshot);
	s->fg_sampled = (flags & SO_TYPE_ANALYZE) == 0 && sift_pacer_fg_sample_begin(&s->fg_t0);
	if (pscan != NULL)
	{
		s->par = (SiftTblF2Parallel *) pscan;
		scan_set_units(s, s->par->units, s->par->nunits);
	}
	else if (by_ordinal)
	{
		s->cursor.view = sift_f2_view(rel, s->store, s->cx);
		s->units_cap = 16;
		s->units = MemoryContextAlloc(s->cx, sizeof(ScanUnit) * s->units_cap);
		s->prefix = MemoryContextAlloc(s->cx, sizeof(uint64) * (s->units_cap + 1));
		scan_units_restart(s);
	}
	s->span_lo = 0;
	s->span_hi = s->total;
	return &s->base;
}

/* The snapshot the rows of a scan that has not read a row yet are seen by */
void
sift_tbl_f2_scan_set_snapshot(TableScanDesc scan, Snapshot snapshot)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	s->base.rs_snapshot = snapshot;
	sift_tbl_f2_vis_begin(&s->vis, s->base.rs_rd, snapshot);
	sift_tbl_f2_predicate_scan(s->base.rs_rd, snapshot);
}

static void
drop_pin(SiftTblF2Scan *s)
{
	if (s->pin != NULL)
		sift_f2_pin_release(s->pin);
	s->pin = NULL;
}

static SiftF2PartPin *
part_pin(SiftTblF2Scan *s, const ScanUnit *u)
{
	if (s->pin == NULL || s->pin_part_id != u->entry.part_id || s->pin->desc.part_gen != u->desc.part_gen || s->pin->desc.header_blk != u->desc.header_blk)
	{
		drop_pin(s);
		s->pin_part_id = u->entry.part_id;
		s->pin = sift_f2_pin_open_scan(s->base.rs_rd, s->store, &u->desc, (uint32) u->entry.part_id);
	}
	return s->pin;
}

static void
drop_granule(SiftTblF2Scan *s)
{
	if (s->gb != NULL)
		sift_f2_gbuf_unref(s->gb);
	s->gb = NULL;
	s->loaded = false;
}

static void
scan_release(SiftTblF2Scan *s)
{
	drop_granule(s);
	drop_pin(s);
	if (s->page != NULL)
		page_drop(s->page);
}

void
sift_tbl_f2_scan_end(TableScanDesc scan)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	if (s->fg_sampled)
		sift_pacer_record_foreground(GetCurrentTimestamp() - s->fg_t0);
	scan_release(s);
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

	scan_release(s);
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
	s->hidden = 0;
	pin = part_pin(s, u);
	if (s->has_range && (pin->tidx[g].tid_max < s->mintid || pin->tidx[g].tid_min > s->maxtid))
		return false;
	s->gb = sift_f2_gbuf_load(s->base.rs_rd, s->store, pin, g);
	sift_col_delta_scan_attach(s->delta, &u->entry, s->gb);
	select_pending(s, &u->entry);
	n = sift_tbl_f2_vis_granule(&s->vis, s->gb, s->pdel, s->sel);
	s->hidden = s->gb->nrows - n;
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
	SiftMorsel	m;

	if (s->par == NULL || !sift_morsel_claim(par_queue(s->par), ParallelWorkerNumber + 1, &m))
		return false;
	s->span_lo = (int64) s->prefix[m.part] + m.first;
	s->span_hi = s->span_lo + m.n;
	s->G = s->span_lo - 1;
	return true;
}

static bool
next_granule(SiftTblF2Scan *s, int step)
{
	for (;;)
	{
		s->G += step;
		while (step > 0 && s->par == NULL && s->G >= s->span_hi && !s->units_complete)
		{
			(void) scan_open_next_part(s);
			s->span_hi = s->total;
		}
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
		if (s->par == NULL && step < 0)
		{
			scan_complete_units(s);
			s->span_hi = s->total;
		}
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
			sift_f2_slot_store(slot, s->base.rs_rd, s->gb, (uint16) r, NULL, NULL, s->rowcx);
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
	s->mintid = sift_tbl_tid_bound_pack(mintid);
	s->maxtid = sift_tbl_tid_bound_pack(maxtid);
	if (s->par == NULL)
	{
		drop_granule(s);
		scan_units_restart(s);
		s->started = false;
		s->span_hi = 0;
	}
}

bool
sift_tbl_f2_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	return scan_next((SiftTblF2Scan *) scan, direction, slot);
}

/* ---- the cursor for ANALYZE ---- */

/* The granules of the table are shared out over its blocks in order, each block the granules whose place falls in its share (none when the blocks outnumber the granules) */
bool
sift_tbl_f2_scan_analyze_granules(TableScanDesc scan, BlockNumber blk, BlockNumber nblocks)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;

	scan_complete_units(s);
	s->an_next = ((uint64) s->total * blk + nblocks - 1) / nblocks;
	s->an_end = ((uint64) s->total * ((uint64) blk + 1) + nblocks - 1) / nblocks;
	return s->an_next < s->an_end;
}

bool
sift_tbl_f2_scan_analyze_next(TableScanDesc scan, TupleTableSlot *slot, double *deadrows)
{
	SiftTblF2Scan *s = (SiftTblF2Scan *) scan;
	int			r;

	for (;;)
	{
		MemoryContextReset(s->rowcx);
		if (s->loaded && (r = next_selected(s, 1)) >= 0)
		{
			s->r = r;
			sift_f2_slot_store(slot, s->base.rs_rd, s->gb, (uint16) r, NULL, NULL, s->rowcx);
			return true;
		}
		if (s->an_next >= s->an_end)
			return false;
		s->G = (int64) s->an_next++;
		(void) load_granule(s, s->G);
		*deadrows += s->hidden;
		s->r = -1;
	}
}

/* ---- the rows of one virtual block: bitmap scans and TABLESAMPLE ---- */

/* MEM[MEM-TBL-05]: struct SiftTblF2Page (5840 B lazy on bitmap/sample scan + patches ctx) */
struct SiftTblF2Page
{
	uint16		n;
	uint16		pos;
	MemoryContext cx;			/* holds a reference of each part map the rows of the page read their patches of column deltas from, as the snapshot sees them */
	OffsetNumber off[SIFT_TBL_ROWS_PER_VBLOCK];
	SiftF2GranuleBuf *gb[SIFT_TBL_ROWS_PER_VBLOCK];
	uint16		row[SIFT_TBL_ROWS_PER_VBLOCK];
	SiftColDeltaPartMap *pm[SIFT_TBL_ROWS_PER_VBLOCK];
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

	SiftColDeltaPartMap *pm = sift_col_delta_scan_part_map(pl->s->delta);

	select_pending(pl->s, entry);
	if (gm != NULL)
		sift_col_delta_part_map_hold_in(pg->cx, pm);
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
		pg->pm[pg->n] = pm;
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
		SiftColDeltaPartMap *pm = pg->pm[i];
		const SiftColDeltaGranMap *gm = pg->gm[i];
		int			j = i - 1;

		for (; j >= 0 && pg->off[j] > off; j--)
		{
			pg->off[j + 1] = pg->off[j];
			pg->gb[j + 1] = pg->gb[j];
			pg->row[j + 1] = pg->row[j];
			pg->pm[j + 1] = pg->pm[j];
			pg->gm[j + 1] = pg->gm[j];
		}
		pg->off[j + 1] = off;
		pg->gb[j + 1] = gb;
		pg->row[j + 1] = row;
		pg->pm[j + 1] = pm;
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
	sift_f2_slot_store(slot, s->base.rs_rd, pg->gb[pg->pos], pg->row[pg->pos], pg->pm[pg->pos], pg->gm[pg->pos], s->rowcx);
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

bool
sift_tbl_f2_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return ItemPointerGetBlockNumber(tid) < nvblocks(scan->rs_rd) && ItemPointerGetOffsetNumber(tid) >= 1 && ItemPointerGetOffsetNumber(tid) <= SIFT_TBL_ROWS_PER_VBLOCK;
}
