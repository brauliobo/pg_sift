/*
 * sift_table_scan.c - sequential, TID range, parallel, bitmap and index scans of a sift_table, TID fetches, ANALYZE sampling
 * and CREATE INDEX (docs/COL-TAM.md section 4). Every row returned goes through sift_tbl_visible.
 *
 * A scan reads, in order, the delta segments (a page copy at a time, rows in slot order) and the column groups (a unit of the
 * engine at a time, sift_table_seg.c). It works on a private copy of the directory taken at begin, protected by its registered
 * snapshot (P4 of sift_store.h). scan_next_version is the one iterator over the stored row versions; what to do with a version
 * (visibility, index build verdict) is the caller's.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "sift_col_kern.h"
#include "sift_table.h"
#include "sift_visibility.h"

/* ---------- parallel scans ---------- */

typedef struct SiftTblParallel
{
	ParallelTableScanDescData base;
	pg_atomic_uint64 next;		/* next unit to hand out: the delta pages, then the superblocks of the column groups */
	uint64		nunits;
	uint64		ndelta;			/* units that are delta pages */
	SiftDirSnapshot dir;		/* the directory every participant reads */
	int			nsegs;
	struct
	{
		uint64		gen;
		BlockNumber hdr;
		uint64		vbase;
		uint32		vcap;
		uint32		npages;
	}			seg[SIFT_MAX_SEGMENTS];
} SiftTblParallel;

static void
par_fill(Relation rel, SiftTblParallel *par)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	par->nsegs = st->ndelta;
	par->nunits = 0;
	for (int i = 0; i < st->ndelta; i++)
	{
		SiftTblSeg *seg = &st->delta[i];

		sift_tbl_seg_load(rel, seg);
		par->seg[i].gen = seg->gen;
		par->seg[i].hdr = seg->hdr;
		par->seg[i].vbase = seg->vbase;
		par->seg[i].vcap = seg->vcap;
		par->seg[i].npages = seg->npages;
		par->nunits += seg->npages;
	}
	par->ndelta = par->nunits;
	par->dir = st->dir;
	par->nunits += SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) / SIFT_SB_PAGES + 1;
	pg_atomic_write_u64(&par->next, 0);
}

Size
sift_tbl_parallelscan_estimate(Relation rel)
{
	return sizeof(SiftTblParallel);
}

Size
sift_tbl_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	SiftTblParallel *par = (SiftTblParallel *) pscan;

#if PG_VERSION_NUM >= 180000
	par->base.phs_locator = rel->rd_locator;
#else
	par->base.phs_relid = RelationGetRelid(rel);
#endif
	par->base.phs_syncscan = false;
	par->base.phs_snapshot_any = false;
	par->base.phs_snapshot_off = 0;
	par_fill(rel, par);
	return sizeof(SiftTblParallel);
}

void
sift_tbl_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	par_fill(rel, (SiftTblParallel *) pscan);
}

/* ---------- the scan ---------- */

typedef struct SiftTblScan
{
	TableScanDescData base;
	SiftTblRel *st;
	BlockNumber sample_blk;		/* TABLESAMPLE: the vblock being sampled, its rows (sorted by offset) in brows, the last offset */
	OffsetNumber sample_max;
	MemoryContext rowcxt;		/* the rows of the TABLESAMPLE block, until the next one */
	TupleDesc	desc;			/* of the rows returned: the slot's, which is the old one while ALTER TABLE rewrites */
	bool		snap_registered;
	int			nseg;
	SiftTblSeg *seg;			/* private copy of the delta segments (page tables complete) */
	uint32		dpos;			/* delta pages passed, in the segments' order: the page in the copy is number dpos - 1 */
	uint32		ndpages;
	bool		fresh;			/* at the start or at the end: a forward scan goes on from the first row, a backward one from the last */
	SiftTblSeg *cur;			/* segment of the page copy */
	BlockNumber curv;			/* vblock of the page copy */
	Page		copy;
	uint32		nrows;
	uint32		ri;
	const SiftTblRec *rec;		/* the delta record of the version last returned */
	SiftDirSnapshot *dir;		/* the directory the scan reads (its own copy, or the leader's) */
	SiftTblGroupScan *g;
	bool		in_groups;		/* the versions come from g */
	bool		groups_done;
	SiftTblParallel *par;
	bool		range;
	ItemPointerData mintid;
	ItemPointerData maxtid;
	/* bitmap */
	HeapTuple  *brows;
	int			nbrows;
	int			bri;
	int			nbcap;
	/* analyze */
	bool		an_started;
	uint64		an_next;		/* visits so far */
	uint64		an_p;			/* physical blocks of the table */
	uint64		an_m;			/* visits the sampler will make at most */
	uint64		an_delta;		/* delta pages */
	uint64	   *an_ranges;		/* [an_nranges][2] vblocks [from, to) of the groups */
	int			an_nranges;
	uint64		an_v;			/* vblocks of the groups */
	HeapTuple	arows[SIFT_TBL_MAX_OFF];	/* the rows of the group vblock read, with the weight of each in the estimate */
	uint64		an_cur;			/* next vblock of the chunk (of the groups) to read */
	uint64		an_end;
	int			narows;
	int			ari;
	double		aweight;
} SiftTblScan;

/* Back to the position before the first row */
static void
scan_rewind(SiftTblScan *s)
{
	s->dpos = 0;
	s->nrows = s->ri = 0;
	s->cur = NULL;
	s->in_groups = s->groups_done = false;
	s->fresh = true;
}

static void
scan_open_state(Relation rel, SiftTblScan *s)
{
	SiftTblRel *st = s->st;

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
	if (s->par)
		s->dir = &s->par->dir;
	else
	{
		s->dir = palloc(sizeof(SiftDirSnapshot));
		*s->dir = st->dir;
	}
	s->nseg = s->par ? s->par->nsegs : st->ndelta;
	s->seg = s->nseg ? palloc0(s->nseg * sizeof(SiftTblSeg)) : NULL;
	for (int i = 0; i < s->nseg; i++)
	{
		if (s->par)
		{
			/* the leader's list, not the directory as it is now: every participant reads the same units */
			s->seg[i].gen = s->par->seg[i].gen;
			s->seg[i].hdr = s->par->seg[i].hdr;
			s->seg[i].vbase = s->par->seg[i].vbase;
			s->seg[i].vcap = s->par->seg[i].vcap;
		}
		else
			s->seg[i] = st->delta[i];
		sift_tbl_seg_load(rel, &s->seg[i]);
		if (s->par)
			s->seg[i].npages = Min(s->seg[i].npages, s->par->seg[i].npages);
	}
	s->ndpages = 0;
	for (int i = 0; i < s->nseg; i++)
		s->ndpages += s->seg[i].npages;
	scan_rewind(s);
}

/* Frees what the state of scan_open_state holds */
static void
scan_close_state(SiftTblScan *s)
{
	if (s->seg)
		pfree(s->seg);
	if (!s->par)
		pfree(s->dir);
	if (s->g)
		sift_tbl_gscan_end(s->g);
	s->seg = NULL;
	s->g = NULL;
}

/* PG18 keeps the bitmap iterator in the scan descriptor; it ends itself when exhausted */
static void
scan_end_bitmap(TableScanDesc scan)
{
#if PG_VERSION_NUM >= 180000
	if ((scan->rs_flags & SO_TYPE_BITMAPSCAN) && !tbm_exhausted(&scan->st.rs_tbmiterator))
		tbm_end_iterate(&scan->st.rs_tbmiterator);
#endif
}

TableScanDesc
sift_tbl_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan,
					uint32 flags)
{
	SiftTblScan *s = palloc0(sizeof(SiftTblScan));

	s->base.rs_rd = rel;
	s->desc = RelationGetDescr(rel);
	s->base.rs_snapshot = snapshot;
	s->base.rs_nkeys = nkeys;
	s->base.rs_key = key;
	s->base.rs_flags = flags;
	s->base.rs_parallel = pscan;
	s->par = (SiftTblParallel *) pscan;
	/* an ANALYZE scan has no snapshot: it judges rows with SnapshotSelf */
	if (snapshot)
	{
		if (IsMVCCSnapshot(snapshot))
		{
			s->base.rs_snapshot = RegisterSnapshot(snapshot);
			s->snap_registered = true;
		}
		PredicateLockRelation(rel, snapshot);
	}
	s->st = sift_tbl_rel(rel);
	s->sample_blk = InvalidBlockNumber;
	s->copy = palloc(BLCKSZ);
	s->rowcxt = AllocSetContextCreate(CurrentMemoryContext, "sift_table scan row", ALLOCSET_DEFAULT_SIZES);
	scan_open_state(rel, s);
	return &s->base;
}

void
sift_tbl_scan_end(TableScanDesc scan)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	scan_end_bitmap(scan);
	if (s->snap_registered)
		UnregisterSnapshot(scan->rs_snapshot);
	if (scan->rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_snapshot);
	scan_close_state(s);
	pfree(s->copy);
	MemoryContextDelete(s->rowcxt);
	pfree(s);
}

void
sift_tbl_scan_rescan(TableScanDesc scan, struct ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync,
					 bool allow_pagemode)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	scan_end_bitmap(scan);
	scan_close_state(s);
	s->nbrows = s->bri = 0;
	s->sample_blk = InvalidBlockNumber;
	scan_open_state(scan->rs_rd, s);
}

/* The group scan lives as long as the scan, whatever context the caller runs in (ALTER TABLE reads in a per-tuple one) */
static SiftTblGroupScan *
scan_gscan(SiftTblScan *s, Snapshot snapshot)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(s));
	SiftTblGroupScan *g = sift_tbl_gscan_begin(s->base.rs_rd, s->desc, s->st, s->dir, snapshot);

	MemoryContextSwitchTo(old);
	return g;
}

typedef enum ScanUnit
{
	UNIT_NONE,
	UNIT_DELTA_PAGE,			/* a delta page copy is in s->copy */
	UNIT_GROUPS					/* s->g reads a range of superblocks of the column groups */
} ScanUnit;

/* Points the group reader at the superblocks [lo, hi) that the TID range (if any) leaves; false when there are none */
static bool
scan_groups(Relation rel, SiftTblScan *s, uint32 lo, uint32 hi)
{
	if (s->range)
	{
		lo = Max(lo, ItemPointerGetBlockNumberNoCheck(&s->mintid) / SIFT_SB_PAGES);
		hi = Min(hi, ItemPointerGetBlockNumberNoCheck(&s->maxtid) / SIFT_SB_PAGES + 1);
	}
	if (lo >= hi)
		return false;
	if (s->g == NULL)
		s->g = scan_gscan(s, s->base.rs_snapshot);
	sift_tbl_gscan_range(s->g, lo, hi);
	s->in_groups = true;
	return true;
}

/* The segment of the delta page number lin (of all the pages in the segments' order) and its page index there */
static SiftTblSeg *
delta_page(SiftTblScan *s, uint32 lin, uint32 *idx)
{
	SiftTblSeg *seg = s->seg;

	while (lin >= seg->npages)
		lin -= (seg++)->npages;
	*idx = lin;
	return seg;
}

/* Reads page idx of seg into the copy; false when there is nothing to scan in it (outside the TID range, or bytes of an overflowing row) */
static bool
load_page(Relation rel, SiftTblScan *s, SiftTblSeg *seg, uint32 idx)
{
	Buffer		buf;
	Page		page;

	if (s->range && (seg->vbase + idx < ItemPointerGetBlockNumberNoCheck(&s->mintid) ||
					 seg->vbase + idx > ItemPointerGetBlockNumberNoCheck(&s->maxtid)))
		return false;
	buf = ReadBuffer(rel, seg->page[idx]);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
	page = BufferGetPage(buf);
	if (TBL_PAGE(page)->flags & SIFT_TBL_PAGE_OVFDATA)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	memcpy(s->copy, page, BLCKSZ);
	UnlockReleaseBuffer(buf);
	s->cur = seg;
	s->curv = seg->vbase + idx;
	s->nrows = TBL_PAGE(s->copy)->nrows;
	s->ri = 0;
	return true;
}

/* The next unit of work: a delta page with rows (into s->copy) or the column groups */
static ScanUnit
scan_next_unit(Relation rel, SiftTblScan *s)
{
	for (;;)
	{
		SiftTblSeg *seg;
		uint32		idx;

		if (s->par)
		{
			uint64		u = pg_atomic_fetch_add_u64(&s->par->next, 1),
						base = 0;
			int			i;

			if (u >= s->par->nunits)
				return UNIT_NONE;
			if (u >= s->par->ndelta)
			{
				if (scan_groups(rel, s, u - s->par->ndelta, u - s->par->ndelta + 1))
					return UNIT_GROUPS;
				continue;
			}
			for (i = 0; i < s->nseg && u >= base + s->par->seg[i].npages; i++)
				base += s->par->seg[i].npages;
			seg = &s->seg[i];
			idx = u - base;
		}
		else
		{
			if (s->dpos >= s->ndpages)
			{
				if (s->groups_done)
					return UNIT_NONE;
				s->groups_done = true;
				return scan_groups(rel, s, 0, PG_UINT32_MAX) ? UNIT_GROUPS : UNIT_NONE;
			}
			seg = delta_page(s, s->dpos++, &idx);
		}
		if (load_page(rel, s, seg, idx))
			return UNIT_DELTA_PAGE;
	}
}

/* The delta page before the one in the copy, backward; UNIT_NONE before the first */
static ScanUnit
scan_prev_unit(Relation rel, SiftTblScan *s)
{
	while (s->dpos > 1)
	{
		uint32		idx;
		SiftTblSeg *seg = delta_page(s, --s->dpos - 1, &idx);

		if (load_page(rel, s, seg, idx))
		{
			s->ri = s->nrows + 1;
			return UNIT_DELTA_PAGE;
		}
	}
	return UNIT_NONE;
}

static bool
scan_in_range(SiftTblScan *s, ItemPointer tid)
{
	return !s->range || (ItemPointerCompare(tid, &s->mintid) >= 0 && ItemPointerCompare(tid, &s->maxtid) <= 0);
}

/* The next stored row version, visible or not: a delta record or a row of a column group (rv->xmin unset). rv->tuple is not set. */
static bool
scan_next_version(Relation rel, SiftTblScan *s, SiftTblRowVer *rv)
{
	for (;;)
	{
		ItemPointerData tid;

		if (s->in_groups)
		{
			if (sift_tbl_gscan_next(s->g, rv))
			{
				sift_key_to_tid(rv->key, &tid);
				if (scan_in_range(s, &tid))
					return true;
				continue;
			}
			s->in_groups = false;
		}
		else if (s->ri < s->nrows)
		{
			s->ri++;
			if ((s->rec = sift_tbl_page_rec(s->copy, s->cur->gen, s->ri)) == NULL)
				continue;
			ItemPointerSet(&tid, s->curv, s->ri);
			if (!scan_in_range(s, &tid))
				continue;
			sift_tbl_rowver(rv, &tid, s->rec);
			return true;
		}
		else if (scan_next_unit(rel, s) == UNIT_NONE)
			return false;
	}
}

/* Backward from the row last returned: the same versions in the opposite order (the column groups from their last row, then the delta pages) */
static bool
scan_prev_version(Relation rel, SiftTblScan *s, SiftTblRowVer *rv)
{
	for (;;)
	{
		ItemPointerData tid;

		if (s->in_groups)
		{
			if (sift_tbl_gscan_prev(s->g, rv))
			{
				sift_key_to_tid(rv->key, &tid);
				if (scan_in_range(s, &tid))
					return true;
				continue;
			}
			s->in_groups = s->groups_done = false;
			s->cur = NULL;
			s->ri = 0;
			s->dpos = s->ndpages + 1;
		}
		else if (s->cur != NULL && s->ri > 1)
		{
			s->ri--;
			if ((s->rec = sift_tbl_page_rec(s->copy, s->cur->gen, s->ri)) == NULL)
				continue;
			ItemPointerSet(&tid, s->curv, s->ri);
			if (!scan_in_range(s, &tid))
				continue;
			sift_tbl_rowver(rv, &tid, s->rec);
			return true;
		}
		else if (scan_prev_unit(rel, s) == UNIT_NONE)
			return false;
	}
}

/* A backward scan that has not read yet starts after the last row */
static void
scan_seek_end(Relation rel, SiftTblScan *s)
{
	s->dpos = s->ndpages + 1;
	s->groups_done = true;
	if ((s->in_groups = scan_groups(rel, s, 0, PG_UINT32_MAX)))
		sift_tbl_gscan_seek_end(s->g);
}

/* The row of the version scan_next_version returned last (palloc'd, t_self set) */
static HeapTuple
scan_version_tuple(Relation rel, SiftTblScan *s, SiftTblRowVer *rv)
{
	ItemPointerData tid;

	if (s->in_groups)
		return sift_tbl_gscan_tuple(s->g);
	sift_key_to_tid(rv->key, &tid);
	sift_tbl_rowver_tuple(rel, rv, s->rec, s->cur->gen, &tid);
	return rv->tuple;
}

/* Hands the version scan_next_version returned last to slot: a row of a column group is decoded when the slot is read */
static void
scan_store(Relation rel, SiftTblScan *s, SiftTblRowVer *rv, TupleTableSlot *slot)
{
	if (s->in_groups && sift_tbl_slot_is_lazy(slot))
	{
		ItemPointerData tid;
		uint32		pos;
		uint32		token = sift_tbl_gscan_cursor(s->g, &tid, &pos);

		sift_tbl_slot_store_group(slot, rel, &tid, s->g, token, pos);
	}
	else
		sift_tbl_store(slot, scan_version_tuple(rel, s, rv));
}

/* The next visible row into slot in the direction of the scan; false at the end */
static bool
scan_next_slot(Relation rel, SiftTblScan *s, TupleTableSlot *slot, bool forward)
{
	SiftTblRowVer rv;

	while (forward ? scan_next_version(rel, s, &rv) : scan_prev_version(rel, s, &rv))
		if ((s->in_groups && sift_tbl_gscan_plain(s->g)) || sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv))
		{
			scan_store(rel, s, &rv, slot);
			return true;
		}
	return false;
}

bool
sift_tbl_scan_next_stored(TableScanDesc scan, SiftTblRowVer *rv)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	if (!scan_next_version(scan->rs_rd, s, rv))
		return false;
	rv->tuple = scan_version_tuple(scan->rs_rd, s, rv);
	return true;
}

bool
sift_tbl_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	bool		forward = ScanDirectionIsForward(direction);

	s->desc = slot->tts_tupleDescriptor;
	if (!forward && s->fresh)
		scan_seek_end(scan->rs_rd, s);
	s->fresh = false;
	if (scan_next_slot(scan->rs_rd, s, slot, forward))
		return true;
	ExecClearTuple(slot);
	scan_rewind(s);
	return false;
}

void
sift_tbl_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	s->range = true;
	s->mintid = *mintid;
	s->maxtid = *maxtid;
}

bool
sift_tbl_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	return sift_tbl_scan_getnextslot(scan, direction, slot);
}

/* ---------- TID fetches ---------- */

typedef struct SiftTblFetch
{
	IndexFetchTableData base;
	SiftTblRel *st;
} SiftTblFetch;

IndexFetchTableData *
sift_tbl_index_fetch_begin(Relation rel)
{
	SiftTblFetch *f = palloc0(sizeof(SiftTblFetch));

	f->base.rel = rel;
	f->st = sift_tbl_rel(rel);
	sift_tbl_refresh(rel, f->st);
	sift_tbl_tomb_sync(rel, f->st);
	return &f->base;
}

void
sift_tbl_index_fetch_reset(IndexFetchTableData *data)
{
}

void
sift_tbl_index_fetch_end(IndexFetchTableData *data)
{
	pfree(data);
}

/* Fetches tid, tests it against snapshot; the row goes into slot. *all_dead: no snapshot will ever see this TID again */
static bool
fetch_visible(Relation rel, SiftTblRel *st, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *all_dead)
{
	SiftTblRowVer rv;
	bool		dead;

	if (all_dead)
		*all_dead = false;
	PredicateLockRelation(rel, snapshot);
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY || snapshot->snapshot_type == SNAPSHOT_SELF)
		sift_tbl_tomb_sync(rel, st);
	if (!sift_tbl_fetch_lazy(rel, st, tid, &rv))
	{
		if (all_dead)
			*all_dead = true;
		return false;
	}
	if (!sift_tbl_visible(rel, st, snapshot, &rv))
	{
		(void) sift_tbl_inserted_visible(snapshot, &rv, &dead);
		if (all_dead)
			*all_dead = dead;
		sift_tbl_free_row(&rv);
		return false;
	}
	if (rv.tuple == NULL && sift_tbl_slot_is_lazy(slot) && sift_tbl_slot_locate(slot, rel, st, tid))
		return true;
	sift_tbl_fetch_tuple(rel, st, tid, &rv);
	sift_tbl_store(slot, rv.tuple);
	return true;
}

bool
sift_tbl_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	SiftTblFetch *f = (SiftTblFetch *) scan;

	*call_again = false;
	return fetch_visible(scan->rel, f->st, tid, snapshot, slot, all_dead);
}

typedef struct VblockTest
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	Page		copy;			/* the delta page, NULL when the vblock is in a column group (frozen rows: the tombstones decide) */
	uint64		gen;
} VblockTest;

static bool
vblock_tid_visible(void *arg, ItemPointer tid)
{
	VblockTest *t = arg;
	SiftTblRowVer rv;
	const SiftTblRec *rec;

	if (t->copy)
	{
		if ((rec = sift_tbl_page_rec(t->copy, t->gen, ItemPointerGetOffsetNumber(tid))) == NULL)
			return false;
		sift_tbl_rowver(&rv, tid, rec);
		return sift_tbl_visible(t->rel, t->st, t->snapshot, &rv);
	}
	rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.delta = false,.xmin = InvalidTransactionId};
	return sift_tbl_visible(t->rel, t->st, t->snapshot, &rv);
}

/* Keeps the offsets of vblock whose row is visible to snapshot; the delta page is read once (the BM25 index's heap check) */
int
sift_tbl_filter_page(Relation rel, Snapshot snapshot, BlockNumber vblock, uint64 *offmask)
{
	PGAlignedBlock copy;
	VblockTest	t = {.rel = rel,.st = sift_tbl_rel(rel),.snapshot = snapshot,.copy = copy.data};

	if (!sift_tbl_load_vblock(rel, t.st, vblock, copy.data, &t.gen))
		t.copy = NULL;
	return sift_vis_filter_mask(vblock, offmask, vblock_tid_visible, &t);
}

bool
sift_tbl_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
	return fetch_visible(rel, st, tid, snapshot, slot, NULL);
}

bool
sift_tbl_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return ItemPointerGetOffsetNumberNoCheck(tid) >= 1 && ItemPointerGetOffsetNumberNoCheck(tid) <= SIFT_TBL_MAX_OFF &&
		ItemPointerGetBlockNumberNoCheck(tid) < sift_tbl_nvblocks(scan->rs_rd);
}

void
sift_tbl_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	Relation	rel = scan->rs_rd;
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftTblRowVer rv;

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
	for (;;)
	{
		const SiftTblTomb *e;
		int			n;
		uint64		succ = 0;

		if (!sift_tbl_fetch_lazy(rel, st, tid, &rv))
			return;
		sift_tbl_free_row(&rv);
		if ((e = sift_tbl_tomb_find(st, rv.key, &n)) != NULL)
			for (int i = 0; i < n; i++)
				if (e[i].kind == SIFT_TBL_TOMB_UPDATE &&
					(TransactionIdIsCurrentTransactionId(e[i].xid) ||
					 (!XidInMVCCSnapshot(e[i].xid, scan->rs_snapshot) && TransactionIdDidCommit(e[i].xid))))
					succ = e[i].succ;
		if (succ == 0)
			return;
		sift_key_to_tid(succ, tid);
	}
}

bool
sift_tbl_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftTblRowVer rv;
	bool		ok;

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
	if (!sift_tbl_fetch_lazy(rel, st, &slot->tts_tid, &rv))
		return false;
	ok = sift_tbl_visible(rel, st, snapshot, &rv);
	sift_tbl_free_row(&rv);
	return ok;
}

/* TIDs no snapshot will see any more: rows that are gone, or whose deleter is removable now */
TransactionId
sift_tbl_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	TransactionId horizon = InvalidTransactionId;

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *d = &delstate->deltids[i];
		TM_IndexStatus *status = &delstate->status[d->id];
		SiftTblRowVer rv;
		SiftTblDel del;

		if (!sift_tbl_fetch_lazy(rel, st, &d->tid, &rv))
		{
			status->knowndeletable = true;
			continue;
		}
		sift_tbl_free_row(&rv);
		sift_tbl_del_state(st, rv.key, &del);
		if ((del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED) &&
			GlobalVisCheckRemovableXid(rel, del.xid))
		{
			status->knowndeletable = true;
			if (!TransactionIdIsValid(horizon) || TransactionIdFollows(del.xid, horizon))
				horizon = del.xid;
		}
	}
	return horizon;
}

/* ---------- bitmap scans ---------- */

static void
bitmap_add(SiftTblScan *s, HeapTuple tup)
{
	if (s->nbrows == s->nbcap)
	{
		s->nbcap = Max(64, s->nbcap * 2);
		s->brows = s->brows ? repalloc(s->brows, s->nbcap * sizeof(HeapTuple)) :
			MemoryContextAlloc(GetMemoryChunkContext(s), s->nbcap * sizeof(HeapTuple));
	}
	s->brows[s->nbrows++] = tup;
}

/* The visible rows of vblock of the column groups at the given offsets (n < 0: all) */
static void
bitmap_group_page(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n)
{
	uint64		mask[SIFT_OFFMASK_WORDS] = {0};
	HeapTuple	row[SIFT_TBL_MAX_OFF];
	int			cnt;

	if (vblock >= SIFT_TBL_NEXT_VBLOCK(&s->dir->meta))
		return;
	if (s->g == NULL)
		s->g = scan_gscan(s, s->base.rs_snapshot);
	for (int i = 0; i < n; i++)
		sift_col_bit_set(mask, offsets[i] - 1);
	cnt = sift_tbl_gscan_block(s->g, vblock, n < 0 ? NULL : mask, row);
	for (int i = 0; i < cnt; i++)
	{
		SiftTblRowVer rv = {.key = sift_key_from_tid(&row[i]->t_self),.delta = false,.xmin = InvalidTransactionId};

		if (sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv))
			bitmap_add(s, row[i]);
		else
			heap_freetuple(row[i]);
	}
}

/* Collects the visible rows of vblock at the given offsets (n < 0: every slot) */
static void
bitmap_page(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n)
{
	uint64		gen;
	int			cnt = n < 0 ? SIFT_TBL_MAX_OFF : n;

	s->nbrows = s->bri = 0;
	if (!sift_tbl_load_vblock(rel, s->st, vblock, s->copy, &gen))
	{
		bitmap_group_page(rel, s, vblock, offsets, n);
		return;
	}
	for (int i = 0; i < cnt; i++)
	{
		OffsetNumber off = n < 0 ? i + 1 : offsets[i];
		const SiftTblRec *rec = sift_tbl_page_rec(s->copy, gen, off);
		ItemPointerData tid;
		SiftTblRowVer rv;

		if (rec == NULL)
			continue;
		ItemPointerSet(&tid, vblock, off);
		sift_tbl_rowver(&rv, &tid, rec);
		if (!sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv))
			continue;
		sift_tbl_rowver_tuple(rel, &rv, rec, gen, &tid);
		bitmap_add(s, rv.tuple);
	}
}

static bool
bitmap_emit(SiftTblScan *s, TupleTableSlot *slot)
{
	if (s->bri >= s->nbrows)
		return false;
	sift_tbl_store(slot, s->brows[s->bri++]);
	return true;
}

/* ---------- TABLESAMPLE ---------- */

static int
cmp_offset(const void *a, const void *b)
{
	return pg_cmp_u16(ItemPointerGetOffsetNumberNoCheck(&(*(const HeapTuple *) a)->t_self),
					  ItemPointerGetOffsetNumberNoCheck(&(*(const HeapTuple *) b)->t_self));
}

/* The vblocks stand for the blocks: the method (SYSTEM) picks one, or they come in order (BERNOULLI); its visible rows are read at once */
bool
sift_tbl_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber nblocks = sift_tbl_nvblocks(scan->rs_rd);
	MemoryContext old;

	if (tsm->NextSampleBlock)
		s->sample_blk = tsm->NextSampleBlock(scanstate, nblocks);
	else
		s->sample_blk = s->sample_blk == InvalidBlockNumber ? 0 : s->sample_blk + 1;
	if (s->sample_blk >= nblocks)
		s->sample_blk = InvalidBlockNumber;
	if (s->sample_blk == InvalidBlockNumber)
		return false;
	MemoryContextReset(s->rowcxt);
	old = MemoryContextSwitchTo(s->rowcxt);
	bitmap_page(scan->rs_rd, s, s->sample_blk, NULL, -1);
	MemoryContextSwitchTo(old);
	qsort(s->brows, s->nbrows, sizeof(HeapTuple), cmp_offset);
	s->sample_max = s->nbrows > 0 ? ItemPointerGetOffsetNumberNoCheck(&s->brows[s->nbrows - 1]->t_self) : 0;
	return true;
}

bool
sift_tbl_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	OffsetNumber off;

	/* (OffsetNumberIsValid evaluates its argument twice) */
	while ((off = scanstate->tsmroutine->NextSampleTuple(scanstate, s->sample_blk, s->sample_max)) != InvalidOffsetNumber)
	{
		CHECK_FOR_INTERRUPTS();
		while (s->bri < s->nbrows && ItemPointerGetOffsetNumberNoCheck(&s->brows[s->bri]->t_self) < off)
			s->bri++;
		if (s->bri < s->nbrows && ItemPointerGetOffsetNumberNoCheck(&s->brows[s->bri]->t_self) == off)
		{
			sift_tbl_slot_store_tuple(slot, s->brows[s->bri++]);
			return true;
		}
	}
	return false;
}

#if PG_VERSION_NUM >= 180000

bool
sift_tbl_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages,
								uint64 *exact_pages)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	for (;;)
	{
		TBMIterateResult tbmres;

		if (bitmap_emit(s, slot))
			return true;
		if (!tbm_iterate(&scan->st.rs_tbmiterator, &tbmres))
			return false;
		CHECK_FOR_INTERRUPTS();
		if (tbmres.lossy)
		{
			bitmap_page(scan->rs_rd, s, tbmres.blockno, NULL, -1);
			(*lossy_pages)++;
		}
		else
		{
			OffsetNumber offsets[MaxHeapTuplesPerPage];
			int			n = tbm_extract_page_tuple(&tbmres, offsets, lengthof(offsets));

			bitmap_page(scan->rs_rd, s, tbmres.blockno, offsets, n);
			(*exact_pages)++;
		}
		*recheck = tbmres.recheck;
	}
}

#else

bool
sift_tbl_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	CHECK_FOR_INTERRUPTS();
	bitmap_page(scan->rs_rd, s, tbmres->blockno, tbmres->ntuples < 0 ? NULL : tbmres->offsets, tbmres->ntuples);
	return s->nbrows > 0;
}

bool
sift_tbl_scan_bitmap_next_tuple(TableScanDesc scan, TBMIterateResult *tbmres, TupleTableSlot *slot)
{
	return bitmap_emit((SiftTblScan *) scan, slot);
}

#endif

/* ---------- ANALYZE ---------- */

/*
 * The ReadStream only says how many blocks the relation is worth (the buffers are released unread, the number of calls is what the
 * sampler counts); the block space of the table is its P physical blocks: the first ones stand for the delta pages, the rest for
 * chunks of the vblocks of the column groups (the vblocks outside the delta segments). The j-th visit of M reads the block
 * floor(j * P / M): a delta page, or the vblocks of its chunk. When M < P a random vblock of the chunk stands for its w vblocks
 * (liverows and deadrows are weighted); when M = P, every vblock is read: the counts are exact, as a heap's. Either way
 * liverows / M * P estimates the rows of the table whatever the layout.
 */
static void
analyze_start(SiftTblScan *s)
{
	Relation	rel = s->base.rs_rd;
	uint64		next = SIFT_TBL_NEXT_VBLOCK(&s->dir->meta),
				at = 0;

	s->an_started = true;
	s->an_p = RelationGetNumberOfBlocks(rel);
	s->an_m = Min(s->an_p, 300 * (uint64) default_statistics_target);
	for (int i = 0; i < s->nseg; i++)
		s->an_delta += s->seg[i].npages;
	/* the vblocks of the groups: [0, next) without the ranges of the delta segments, which the directory lists in ascending order */
	s->an_ranges = palloc((s->nseg + 1) * 2 * sizeof(uint64));
	for (int i = 0; i <= s->nseg; i++)
	{
		uint64		end = i < s->nseg ? s->seg[i].vbase : next;

		if (end > at)
		{
			s->an_ranges[2 * s->an_nranges] = at;
			s->an_ranges[2 * s->an_nranges + 1] = end;
			s->an_nranges++;
			s->an_v += end - at;
		}
		if (i < s->nseg)
			at = s->seg[i].vbase + s->seg[i].vcap;
	}
}

/* The vblock number of the n-th vblock of the groups */
static BlockNumber
analyze_group_vblock(const SiftTblScan *s, uint64 n)
{
	for (int i = 0; i < s->an_nranges; i++)
	{
		uint64		len = s->an_ranges[2 * i + 1] - s->an_ranges[2 * i];

		if (n < len)
			return s->an_ranges[2 * i] + n;
		n -= len;
	}
	elog(ERROR, "sift_table: vblock %llu of the column groups is out of range", (unsigned long long) n);
	return 0;
}

/* The vblocks of the groups that chunk c of them stands for: all of them or a random one with the weight of the chunk */
static void
analyze_group_block(SiftTblScan *s, uint64 c)
{
	uint64		g = Max(s->an_p - s->an_delta, 1),
				from = c * s->an_v / g,
				to = (c + 1) * s->an_v / g;

	s->an_cur = s->an_end = from;
	s->aweight = 1;
	if (to <= from)
		return;
	if (s->an_m == s->an_p)
		s->an_end = to;
	else
	{
		s->aweight = (double) (to - from);
		s->an_cur = from + (c * UINT64CONST(2654435761)) % (to - from);
		s->an_end = s->an_cur + 1;
	}
}

/* The rows of the next vblock of the chunk into s->arows */
static void
analyze_load_vblock(SiftTblScan *s)
{
	if (s->g == NULL)
		s->g = scan_gscan(s, SnapshotSelf);
	s->ari = 0;
	s->narows = sift_tbl_gscan_block(s->g, analyze_group_vblock(s, s->an_cur++), NULL, s->arows);
}

bool
sift_tbl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	Buffer		buf = read_stream_next_buffer(stream, NULL);
	uint64		b,
				base = 0;
	int			i;
	SiftTblSeg *seg;

	if (!BufferIsValid(buf))
		return false;
	ReleaseBuffer(buf);
	if (!s->an_started)
		analyze_start(s);
	s->nrows = s->ri = 0;
	s->narows = s->ari = 0;
	s->an_cur = s->an_end = 0;
	s->aweight = 1;
	b = s->an_next++ * s->an_p / s->an_m;
	if (b >= s->an_delta)
	{
		analyze_group_block(s, b - s->an_delta);
		return true;
	}
	for (i = 0; b >= base + s->seg[i].npages; i++)
		base += s->seg[i].npages;
	seg = &s->seg[i];
	buf = ReadBuffer(scan->rs_rd, seg->page[b - base]);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(scan->rs_rd, buf, SIFT_PT_MUTDATA, seg->gen);
	if (!(TBL_PAGE(BufferGetPage(buf))->flags & SIFT_TBL_PAGE_OVFDATA))
	{
		memcpy(s->copy, BufferGetPage(buf), BLCKSZ);
		s->cur = seg;
		s->curv = seg->vbase + (b - base);
		s->nrows = TBL_PAGE(s->copy)->nrows;
	}
	UnlockReleaseBuffer(buf);
	return true;
}

bool
sift_tbl_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	Relation	rel = scan->rs_rd;

	while (s->ri < s->nrows)
	{
		const SiftTblRec *rec = sift_tbl_page_rec(s->copy, s->cur->gen, ++s->ri);
		ItemPointerData tid;
		SiftTblRowVer rv;

		if (rec == NULL)
			continue;
		ItemPointerSet(&tid, s->curv, s->ri);
		sift_tbl_rowver(&rv, &tid, rec);
		if (!sift_tbl_visible(rel, s->st, SnapshotSelf, &rv))
		{
			*deadrows += 1;
			continue;
		}
		sift_tbl_rowver_tuple(rel, &rv, rec, s->cur->gen, &tid);
		sift_tbl_store(slot, rv.tuple);
		*liverows += 1;
		return true;
	}
	while (s->ari < s->narows || s->an_cur < s->an_end)
	{
		HeapTuple	tup;
		SiftTblRowVer rv;

		if (s->ari >= s->narows)
		{
			analyze_load_vblock(s);
			continue;
		}
		tup = s->arows[s->ari++];
		rv = (SiftTblRowVer) {.key = sift_key_from_tid(&tup->t_self),.delta = false,.xmin = InvalidTransactionId};
		if (!sift_tbl_visible(rel, s->st, SnapshotSelf, &rv))
		{
			*deadrows += s->aweight;
			heap_freetuple(tup);
			continue;
		}
		sift_tbl_store(slot, tup);
		*liverows += s->aweight;
		return true;
	}
	return false;
}

/* ---------- CREATE INDEX ---------- */

/*
 * How a version counts for an index build: the rules of heapam_index_build_range_scan. Writers are excluded by the lock of a
 * plain build, so another transaction's in-progress work only shows in a concurrent build, which uses an MVCC snapshot.
 */
typedef enum BuildVerdict
{
	BUILD_SKIP,
	BUILD_LIVE,
	BUILD_RECENTLY_DEAD
} BuildVerdict;

static BuildVerdict
build_verdict(Relation rel, SiftTblRel *st, const SiftTblRowVer *rv, TransactionId oldest, bool anyvisible, bool mvcc,
			  Snapshot snap)
{
	SiftTblDel	del;
	bool		dead;

	if (mvcc)
		return sift_tbl_visible(rel, st, snap, rv) ? BUILD_LIVE : BUILD_SKIP;
	if (TransactionIdIsNormal(rv->xmin))
	{
		if (rv->flags & SIFT_TBL_REC_DEAD)
			return BUILD_SKIP;
		if (!TransactionIdIsCurrentTransactionId(rv->xmin) && !TransactionIdDidCommit(rv->xmin))
		{
			if (TransactionIdIsInProgress(rv->xmin))
			{
				if (!anyvisible)
					XactLockTableWait(rv->xmin, rel, NULL, XLTW_InsertIndexUnique);
				else
					return BUILD_LIVE;
				if (!TransactionIdDidCommit(rv->xmin))
					return BUILD_SKIP;
			}
			else
				return BUILD_SKIP;
		}
	}
	(void) dead;
	sift_tbl_del_state(st, rv->key, &del);
	switch (del.state)
	{
		case SIFT_TBL_DEL_NONE:
		case SIFT_TBL_DEL_INPROGRESS:
			return BUILD_LIVE;
		case SIFT_TBL_DEL_SELF:
			return BUILD_RECENTLY_DEAD;
		default:
			return TransactionIdPrecedes(del.xid, oldest) ? BUILD_SKIP : BUILD_RECENTLY_DEAD;
	}
}

double
sift_tbl_index_build_range_scan(Relation table_rel, Relation index_rel, IndexInfo *indexInfo, bool allow_sync,
								bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks,
								IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	SiftTblScan *s;
	double		reltuples = 0;
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	Snapshot	snapshot;
	bool		mvcc,
				temp_snap = false;
	TransactionId oldest = InvalidTransactionId;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointerData tid;
	BlockNumber end = numblocks == InvalidBlockNumber ? InvalidBlockNumber : start_blockno + numblocks;

	econtext->ecxt_scantuple = slot;
	indexInfo->ii_PredicateState = ExecPrepareQual(indexInfo->ii_Predicate, estate);
	if (scan == NULL)
	{
		if (indexInfo->ii_Concurrent)
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			temp_snap = true;
		}
		else
		{
			snapshot = SnapshotAny;
			oldest = GetOldestNonRemovableTransactionId(table_rel);
		}
		scan = sift_tbl_scan_begin(table_rel, snapshot, 0, NULL, NULL, SO_TYPE_SEQSCAN | SO_ALLOW_STRAT);
	}
	else
	{
		snapshot = scan->rs_snapshot;
		oldest = snapshot == SnapshotAny ? GetOldestNonRemovableTransactionId(table_rel) : InvalidTransactionId;
	}
	mvcc = IsMVCCSnapshot(snapshot);
	s = (SiftTblScan *) scan;
	for (;;)
	{
		SiftTblRowVer rv;
		BuildVerdict v;

		if (!scan_next_version(table_rel, s, &rv))
			break;
		if (rv.key >> 16 < start_blockno || rv.key >> 16 >= end)
			continue;
		CHECK_FOR_INTERRUPTS();
		if ((v = build_verdict(table_rel, s->st, &rv, oldest, anyvisible, mvcc, snapshot)) == BUILD_SKIP)
			continue;
		scan_store(table_rel, s, &rv, slot);
		reltuples += 1;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (indexInfo->ii_PredicateState != NULL && !ExecQual(indexInfo->ii_PredicateState, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		sift_key_to_tid(rv.key, &tid);
		callback(index_rel, &tid, values, isnull, v == BUILD_LIVE, callback_state);
	}
	sift_tbl_scan_end(scan);			/* a scan handed in is ended here too, as heapam does */
	if (temp_snap)
		UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
	return reltuples;
}
