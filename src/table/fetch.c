/*
 * table/fetch.c - TID fetches, bitmap scans and TABLESAMPLE of a sift_table: the rows read by TID or by vblock
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
#include "sift_visibility.h"
#include "table/scan_int.h"

/* ---------- TID fetches ---------- */

typedef struct SiftTblFetch
{
	IndexFetchTableData base;
	SiftTblRel *st;
} SiftTblFetch;

IndexFetchTableData *
sift_tbl_index_fetch_begin(Relation rel SIFT_TAM_FLAGS_PARAM)
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
	Page		copy;			/* the write buffer page, NULL when the vblock is in a column group (the tombstones decide, and the insertion of a row with a version) */
	uint64		gen;
	SiftTblVerPage vp;			/* the versions of the rows of the page of a unit with unfrozen rows */
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
	rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false};
	sift_tbl_vers_rowver(&t->vp, ItemPointerGetOffsetNumber(tid), &rv.xmin, &rv.cmin);
	return sift_tbl_visible(t->rel, t->st, t->snapshot, &rv);
}

/* Keeps the offsets of vblock whose row is visible to snapshot; the write buffer page is read once; a unit the snapshot sees no insertion of is no row at all */
int
sift_tbl_filter_page_vers(Relation rel, Snapshot snapshot, SiftTblVers *vers, BlockNumber vblock, uint64 *offmask)
{
	PGAlignedBlock copy;
	VblockTest	t = {.rel = rel,.st = sift_tbl_rel(rel),.snapshot = snapshot,.copy = copy.data};

	if (!sift_tbl_load_vblock(rel, t.st, vblock, copy.data, &t.gen))
	{
		SiftTblVerState state = sift_tbl_vers_sb(rel, snapshot, vers, vblock / SIFT_SB_PAGES);

		t.copy = NULL;
		if (state == SIFT_TBL_VER_NONE)
		{
			memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
			return 0;
		}
		if (state == SIFT_TBL_VER_ROWS)
			sift_tbl_vers_page(vers, vblock, &t.vp);
	}
	return sift_vis_filter_mask(vblock, offmask, vblock_tid_visible, &t);
}

/* The same for a reader that goes by the directory this backend holds (the BM25 index's heap check) */
int
sift_tbl_filter_page(Relation rel, Snapshot snapshot, BlockNumber vblock, uint64 *offmask)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (st->curvers == NULL || st->curvers_version != st->dir.version || st->curvers_rel != rel)
	{
		if (st->curvers_cxt)
			MemoryContextDelete(st->curvers_cxt);
		st->curvers_cxt = AllocSetContextCreate(st->cxt, "sift_table versions of the directory", ALLOCSET_SMALL_SIZES);
		st->curvers = sift_tbl_vers_build(rel, st, &st->dir, st->curvers_cxt);
		st->curvers_version = st->dir.version;
		st->curvers_rel = rel;
	}
	return sift_tbl_filter_page_vers(rel, snapshot, st->curvers, vblock, offmask);
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
	int			final = 0;

	sift_tbl_refresh(rel, st);
	sift_tbl_tomb_sync(rel, st);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *d = &delstate->deltids[i];
		TM_IndexStatus *status = &delstate->status[d->id];
		SiftTblRowVer rv;
		SiftTblDel del;

		if (!sift_tbl_fetch_lazy(rel, st, &d->tid, &rv))
			status->knowndeletable = true;
		else
		{
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
		if (status->knowndeletable)
			final = i + 1;
	}
	/* the array ends at its last deletable entry (none left: a bottom-up pass found nothing) */
	delstate->ndeltids = final;
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
	TransactionId xmin[SIFT_TBL_MAX_OFF];
	CommandId	cmin[SIFT_TBL_MAX_OFF];
	int			cnt;

	if (vblock >= SIFT_TBL_NEXT_VBLOCK(&s->dir->meta))
		return;
	if (s->g == NULL)
		s->g = scan_gscan(s, s->base.rs_snapshot);
	for (int i = 0; i < n; i++)
		sift_col_bit_set(mask, offsets[i] - 1);
	cnt = sift_tbl_gscan_block(s->g, vblock, n < 0 ? NULL : mask, row, xmin, cmin);
	for (int i = 0; i < cnt; i++)
	{
		SiftTblRowVer rv = {.key = sift_key_from_tid(&row[i]->t_self),.wbuf = false,.xmin = xmin[i],.cmin = cmin[i]};

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
