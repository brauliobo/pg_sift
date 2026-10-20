/*
 * table/scan.c - sequential, TID range, parallel, bitmap and index scans of a sift_table, TID fetches, ANALYZE sampling
 * and CREATE INDEX (docs/COL-TAM.md section 4). Every row returned goes through sift_tbl_visible.
 *
 * A scan reads, in order, the write buffers (a page copy at a time, rows in slot order) and the column groups (a unit of the
 * engine at a time, table/gscan.c). It works on a private copy of the directory taken at begin, protected by its registered
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

#include "col/kern.h"
#include "table/table.h"
#include "table/scan_int.h"
#include "sift_visibility.h"

/* ---------- parallel scans ---------- */

static void
par_fill(Relation rel, SiftTblParallel *par)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	par->nsegs = st->nwbuf;
	par->nunits = 0;
	for (int i = 0; i < st->nwbuf; i++)
	{
		SiftTblSeg *seg = &st->wbuf[i];

		sift_tbl_seg_load(rel, seg);
		par->seg[i].gen = seg->gen;
		par->seg[i].hdr = seg->hdr;
		par->seg[i].vbase = seg->vbase;
		par->seg[i].vcap = seg->vcap;
		par->seg[i].npages = seg->npages;
		par->nunits += seg->npages;
	}
	par->nwbuf = par->nunits;
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
	s->nseg = s->par ? s->par->nsegs : st->nwbuf;
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
			s->seg[i] = st->wbuf[i];
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

typedef enum ScanUnit
{
	UNIT_NONE,
	UNIT_WBUF_PAGE,			/* a write buffer page copy is in s->copy */
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

/* The segment of the write buffer page number lin (of all the pages in the segments' order) and its page index there */
static SiftTblSeg *
wbuf_page(SiftTblScan *s, uint32 lin, uint32 *idx)
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
	sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
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

/* The next unit of work: a write buffer page with rows (into s->copy) or the column groups */
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
			if (u >= s->par->nwbuf)
			{
				if (scan_groups(rel, s, u - s->par->nwbuf, u - s->par->nwbuf + 1))
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
			seg = wbuf_page(s, s->dpos++, &idx);
		}
		if (load_page(rel, s, seg, idx))
			return UNIT_WBUF_PAGE;
	}
}

/* The write buffer page before the one in the copy, backward; UNIT_NONE before the first */
static ScanUnit
scan_prev_unit(Relation rel, SiftTblScan *s)
{
	while (s->dpos > 1)
	{
		uint32		idx;
		SiftTblSeg *seg = wbuf_page(s, --s->dpos - 1, &idx);

		if (load_page(rel, s, seg, idx))
		{
			s->ri = s->nrows + 1;
			return UNIT_WBUF_PAGE;
		}
	}
	return UNIT_NONE;
}

static bool
scan_in_range(SiftTblScan *s, ItemPointer tid)
{
	return !s->range || (ItemPointerCompare(tid, &s->mintid) >= 0 && ItemPointerCompare(tid, &s->maxtid) <= 0);
}

/* The next stored row version, visible or not: a write buffer record or a row of a column group (rv->xmin unset). rv->tuple is not set. */
bool
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

/* Backward from the row last returned: the same versions in the opposite order (the column groups from their last row, then the write buffer pages) */
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
void
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
