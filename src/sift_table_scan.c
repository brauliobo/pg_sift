/*
 * sift_table_scan.c - sequential, TID range, parallel, bitmap and index scans of a sift_table, TID fetches, ANALYZE sampling
 * and CREATE INDEX (docs/COL-TAM.md section 4). Every row returned goes through sift_tbl_visible.
 *
 * A scan reads, in order, the delta segments (a page copy at a time, rows in slot order) and the column groups (engine glue).
 * It works on a private copy of the directory taken at begin, protected by its registered snapshot (P4 of sift_store.h).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "sift_table.h"
#include "sift_visibility.h"

/* ---------- parallel scans ---------- */

typedef struct SiftTblParallel
{
	ParallelTableScanDescData base;
	pg_atomic_uint64 next;		/* next unit (a delta page) to hand out */
	uint64		nunits;
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
	bool		snap_registered;
	int			nseg;
	SiftTblSeg *seg;			/* private copy of the delta segments (page tables complete) */
	int			si;				/* segment of the next page */
	uint32		pi;				/* next page of it */
	SiftTblSeg *cur;			/* segment of the page copy */
	BlockNumber curv;			/* vblock of the page copy */
	Page		copy;
	uint32		nrows;
	uint32		ri;
	uint64		unit;			/* parallel: unit of the page last taken */
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
	uint64		an_units;
	uint64		an_next;
	uint64		an_step;
	bool		an_started;
} SiftTblScan;

static void
scan_open_state(Relation rel, SiftTblScan *s)
{
	SiftTblRel *st = s->st;

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
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
	s->si = 0;
	s->pi = 0;
	s->nrows = s->ri = 0;
	s->cur = NULL;
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
	s->copy = palloc(BLCKSZ);
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
	if (s->seg)
		pfree(s->seg);
	pfree(s->copy);
	pfree(s);
}

void
sift_tbl_scan_rescan(TableScanDesc scan, struct ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync,
					 bool allow_pagemode)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	scan_end_bitmap(scan);
	if (s->seg)
		pfree(s->seg);
	s->nbrows = s->bri = 0;
	scan_open_state(scan->rs_rd, s);
}

/* The next delta page with rows into s->copy; false at the end */
static bool
scan_next_page(Relation rel, SiftTblScan *s)
{
	for (;;)
	{
		SiftTblSeg *seg;
		BlockNumber blk;
		Buffer		buf;
		Page		page;
		uint32		idx;

		if (s->par)
		{
			uint64		u = pg_atomic_fetch_add_u64(&s->par->next, 1),
						base = 0;
			int			i;

			if (u >= s->par->nunits)
				return false;
			for (i = 0; i < s->nseg && u >= base + s->par->seg[i].npages; i++)
				base += s->par->seg[i].npages;
			seg = &s->seg[i];
			idx = u - base;
		}
		else
		{
			if (s->si >= s->nseg)
				return false;
			seg = &s->seg[s->si];
			if (s->pi >= seg->npages)
			{
				s->si++;
				s->pi = 0;
				continue;
			}
			idx = s->pi++;
		}
		if (s->range && (seg->vbase + idx < ItemPointerGetBlockNumberNoCheck(&s->mintid) ||
						 seg->vbase + idx > ItemPointerGetBlockNumberNoCheck(&s->maxtid)))
			continue;
		blk = seg->page[idx];
		buf = ReadBuffer(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
		page = BufferGetPage(buf);
		if (TBL_PAGE(page)->flags & SIFT_TBL_PAGE_OVFDATA)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		memcpy(s->copy, page, BLCKSZ);
		UnlockReleaseBuffer(buf);
		s->cur = seg;
		s->curv = seg->vbase + idx;
		s->nrows = TBL_PAGE(s->copy)->nrows;
		s->ri = 0;
		return true;
	}
}

/* The next visible row of the delta pages, as a HeapTuple (palloc'd, t_self set); NULL at the end */
static HeapTuple
scan_next_row(Relation rel, SiftTblScan *s)
{
	Snapshot	snap = s->base.rs_snapshot;

	for (;;)
	{
		const SiftTblRec *rec;
		SiftTblRowVer rv;
		ItemPointerData tid;

		while (s->ri >= s->nrows)
			if (!scan_next_page(rel, s))
				return NULL;
		s->ri++;
		if ((rec = sift_tbl_page_rec(s->copy, s->cur->gen, s->ri)) == NULL)
			continue;
		ItemPointerSet(&tid, s->curv, s->ri);
		if (s->range && (ItemPointerCompare(&tid, &s->mintid) < 0 || ItemPointerCompare(&tid, &s->maxtid) > 0))
			continue;
		sift_tbl_rowver(&rv, &tid, rec);
		if (!sift_tbl_visible(rel, s->st, snap, &rv))
			continue;
		sift_tbl_rowver_tuple(rel, &rv, rec, s->cur->gen, &tid);
		return rv.tuple;
	}
}

bool
sift_tbl_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	HeapTuple	tup;

	if (!ScanDirectionIsForward(direction))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_table tables do not support backward scans")));
	if ((tup = scan_next_row(scan->rs_rd, s)) == NULL)
	{
		ExecClearTuple(slot);
		return false;
	}
	sift_tbl_store(slot, tup);
	return true;
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
	if (!sift_tbl_fetch_row(rel, st, tid, &rv))
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
		heap_freetuple(rv.tuple);
		return false;
	}
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
	Page		copy;			/* the delta page, NULL when the vblock is in a column group */
	uint64		gen;
} VblockTest;

static bool
vblock_tid_visible(void *arg, ItemPointer tid)
{
	VblockTest *t = arg;
	SiftTblRowVer rv;
	const SiftTblRec *rec;
	bool		ok;

	if (t->copy)
	{
		if ((rec = sift_tbl_page_rec(t->copy, t->gen, ItemPointerGetOffsetNumber(tid))) == NULL)
			return false;
		sift_tbl_rowver(&rv, tid, rec);
		return sift_tbl_visible(t->rel, t->st, t->snapshot, &rv);
	}
	if (!sift_tbl_fetch_row(t->rel, t->st, tid, &rv))
		return false;
	ok = sift_tbl_visible(t->rel, t->st, t->snapshot, &rv);
	heap_freetuple(rv.tuple);
	return ok;
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

		if (!sift_tbl_fetch_row(rel, st, tid, &rv))
			return;
		heap_freetuple(rv.tuple);
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
	if (!sift_tbl_fetch_row(rel, st, &slot->tts_tid, &rv))
		return false;
	ok = sift_tbl_visible(rel, st, snapshot, &rv);
	heap_freetuple(rv.tuple);
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

		if (!sift_tbl_fetch_row(rel, st, &d->tid, &rv))
		{
			status->knowndeletable = true;
			continue;
		}
		heap_freetuple(rv.tuple);
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
		s->brows = s->brows ? repalloc(s->brows, s->nbcap * sizeof(HeapTuple)) : palloc(s->nbcap * sizeof(HeapTuple));
	}
	s->brows[s->nbrows++] = tup;
}

/* Collects the visible rows of vblock at the given offsets (n < 0: every slot) */
static void
bitmap_page(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n)
{
	uint64		gen;
	int			cnt = n < 0 ? SIFT_TBL_MAX_OFF : n;

	s->nbrows = s->bri = 0;
	if (!sift_tbl_load_vblock(rel, s->st, vblock, s->copy, &gen))
		return;
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
 * The ReadStream only says how many sampled blocks the relation is worth (the buffers are released unread, the number of calls
 * is what the sampler counts); the sampling units are the delta pages: all of them, or every U/m-th when there are more than m.
 */
bool
sift_tbl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	Buffer		buf = read_stream_next_buffer(stream, NULL);
	uint64		u,
				base = 0;
	int			i;
	SiftTblSeg *seg;

	if (!BufferIsValid(buf))
		return false;
	ReleaseBuffer(buf);
	if (!s->an_started)
	{
		s->an_started = true;
		for (i = 0; i < s->nseg; i++)
			s->an_units += s->seg[i].npages;
		s->an_step = Min(RelationGetNumberOfBlocks(scan->rs_rd), 300 * (uint64) default_statistics_target);
	}
	s->nrows = s->ri = 0;
	u = s->an_units <= s->an_step ? s->an_next : s->an_next * s->an_units / s->an_step;
	s->an_next++;
	if (u >= s->an_units)
		return true;			/* more blocks than units: nothing in this one */
	for (i = 0; u >= base + s->seg[i].npages; i++)
		base += s->seg[i].npages;
	seg = &s->seg[i];
	buf = ReadBuffer(scan->rs_rd, seg->page[u - base]);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(scan->rs_rd, buf, SIFT_PT_MUTDATA, seg->gen);
	if (!(TBL_PAGE(BufferGetPage(buf))->flags & SIFT_TBL_PAGE_OVFDATA))
	{
		memcpy(s->copy, BufferGetPage(buf), BLCKSZ);
		s->cur = seg;
		s->curv = seg->vbase + (u - base);
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
	HeapTuple	tup;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
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
		const SiftTblRec *rec;
		SiftTblRowVer rv;
		ItemPointerData tid;
		BuildVerdict v;

		while (s->ri >= s->nrows)
			if (!scan_next_page(table_rel, s))
				goto done;
		s->ri++;
		if ((rec = sift_tbl_page_rec(s->copy, s->cur->gen, s->ri)) == NULL)
			continue;
		if (s->curv < start_blockno || s->curv >= end)
			continue;
		CHECK_FOR_INTERRUPTS();
		ItemPointerSet(&tid, s->curv, s->ri);
		sift_tbl_rowver(&rv, &tid, rec);
		if ((v = build_verdict(table_rel, s->st, &rv, oldest, anyvisible, mvcc, snapshot)) == BUILD_SKIP)
			continue;
		sift_tbl_rowver_tuple(table_rel, &rv, rec, s->cur->gen, &tid);
		tup = rv.tuple;
		sift_tbl_store(slot, tup);
		reltuples += 1;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (indexInfo->ii_PredicateState != NULL && !ExecQual(indexInfo->ii_PredicateState, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		callback(index_rel, &tid, values, isnull, v == BUILD_LIVE, callback_state);
	}
done:
	sift_tbl_scan_end(scan);			/* a scan handed in is ended here too, as heapam does */
	if (temp_snap)
		UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
	return reltuples;
}
