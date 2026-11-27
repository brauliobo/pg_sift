/*
 * table/f2/mvcc.c - what a snapshot sees of a row of a sift_table, for every kind of snapshot.
 *
 * A row is inserted by (xmin, cmin) and dead by the delete of its deletion vector entry or of a pending delete (xmax, cmax). A granule of one inserter keeps no version bytes
 * (SIFT_GDESC_SINGLE_XACT), a frozen one neither, and a sealed part older than the snapshot is asked nothing. The transactions are asked of the log once each (SiftXidCache), and a
 * SERIALIZABLE transaction is told of the writers it cannot see.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"

#include "table/f2/chain_cache.h"
#include "table/f2/gcache.h"
#include "table/f2/tbl.h"
#include "core/compat.h"

static uint64 pd_epochs = 0;

void
sift_tbl_f2_vis_begin(SiftTblVis *v, Relation rel, Snapshot snap)
{
	v->rel = rel;
	v->snap = snap;
	sift_xid_cache_init(&v->xc, snap);
	v->snap_xmin = sift_vis_snap_xmin(snap);
	v->serializable = IsolationIsSerializable() && CheckForSerializableConflictOutNeeded(rel, snap);
	v->pd_epoch = snap->snapshot_type == SNAPSHOT_MVCC ? ++pd_epochs : 0;
	v->pd_cid = snap->curcid;
}

static bool
mvcc_like(Snapshot snap)
{
	return snap->snapshot_type == SNAPSHOT_MVCC || snap->snapshot_type == SNAPSHOT_HISTORIC_MVCC || snap->snapshot_type == SNAPSHOT_TOAST;
}

static void
conflict_out(SiftTblVis *v, TransactionId xid)
{
	if (v->serializable && TransactionIdIsNormal(xid) && !TransactionIdIsCurrentTransactionId(xid))
		CheckForSerializableConflictOut(v->rel, xid, v->snap);
}

/* The transaction did not roll back. A commit is final, so the log is asked first and the process array (ProcArrayLock) only of one the log has not seen commit, the log again after
 * it for one that committed in between */
static bool
not_aborted(TransactionId xid)
{
	return TransactionIdDidCommit(xid) || TransactionIdIsCurrentTransactionId(xid) || TransactionIdIsInProgress(xid) || TransactionIdDidCommit(xid);
}

/* The insert of (xmin, cmin) is there for the snapshot */
static bool
inserted(SiftTblVis *v, TransactionId xmin, CommandId cmin)
{
	Snapshot	snap = v->snap;

	if (!TransactionIdIsNormal(xmin))
		return true;
	switch (snap->snapshot_type)
	{
		case SNAPSHOT_SELF:
			return TransactionIdIsCurrentTransactionId(xmin) || TransactionIdDidCommit(xmin);
		case SNAPSHOT_ANY:
			return not_aborted(xmin);
		case SNAPSHOT_DIRTY:
			if (TransactionIdIsCurrentTransactionId(xmin))
				return true;
			if (TransactionIdIsInProgress(xmin))
			{
				snap->xmin = xmin;
				return true;
			}
			return TransactionIdDidCommit(xmin);
		case SNAPSHOT_NON_VACUUMABLE:
			return !TransactionIdDidAbort(xmin) || TransactionIdIsInProgress(xmin);
		default:
			if (sift_vis_inserted_cached(&v->xc, xmin, cmin))
				return true;
			conflict_out(v, xmin);
			return false;
	}
}

/* A column delta never touches a column of an index, so a dirty snapshot has nothing to wait for in a writer that is running: the value is the committed one, which the log tells
 * without the process array. So is it for a snapshot that takes any row: the old row an UPDATE makes its new one from carries no value of a writer that may still roll back */
bool
sift_tbl_f2_delta_visible(SiftTblVis *v, TransactionId xmin, CommandId cid)
{
	if (v->snap->snapshot_type == SNAPSHOT_DIRTY || v->snap->snapshot_type == SNAPSHOT_ANY)
		return !TransactionIdIsNormal(xmin) || TransactionIdIsCurrentTransactionId(xmin) || TransactionIdDidCommit(xmin);
	return inserted(v, xmin, cid);
}

/* The delete by (xmax, cmax) has happened for the snapshot: the row is gone for it */
static bool
deleted(SiftTblVis *v, TransactionId xmax, CommandId cmax)
{
	Snapshot	snap = v->snap;

	if (!TransactionIdIsValid(xmax))
		return snap->snapshot_type != SNAPSHOT_ANY;
	switch (snap->snapshot_type)
	{
		case SNAPSHOT_SELF:
			return TransactionIdIsCurrentTransactionId(xmax) || TransactionIdDidCommit(xmax);
		case SNAPSHOT_ANY:
			return false;
		case SNAPSHOT_DIRTY:
			if (TransactionIdIsCurrentTransactionId(xmax))
				return true;
			if (TransactionIdIsInProgress(xmax))
			{
				snap->xmax = xmax;
				return false;
			}
			return TransactionIdDidCommit(xmax);
		case SNAPSHOT_NON_VACUUMABLE:
			return TransactionIdDidCommit(xmax) && sift_vis_removable_xid(snap->vistest, xmax);
		default:
			if (sift_vis_deleted_cached(&v->xc, xmax, cmax))
				return true;
			conflict_out(v, xmax);
			return false;
	}
}

/* ---- pending deletes ---- */

SiftPendDelIndex *
sift_tbl_f2_pending_index(Relation rel, const SiftPartitionEntry *entry, Snapshot snap)
{
	SiftTblVis	v;
	SiftPendDelRec *recs = NULL;
	const SiftTxnStage *st = sift_stage_peek(rel);
	uint64	   *dead;
	int			n = 0,
				nstaged = st != NULL ? st->ndels : 0,
				ndead = 0;
	SiftPendDelIndex *px;

	if ((entry->pend_del_blk == InvalidBlockNumber && nstaged == 0) || snap->snapshot_type == SNAPSHOT_ANY)
		return NULL;
	sift_tbl_f2_vis_begin(&v, rel, snap);
	if (entry->pend_del_blk != InvalidBlockNumber)
		recs = sift_pend_del_read(rel, entry->part_id, entry->pend_del_blk, &n, NULL, NULL);
	dead = palloc(sizeof(uint64) * Max(n + nstaged, 1));
	for (int i = 0; i < n; i++)
		if ((recs[i].tid & SIFT_PD_TID_SUCC) == 0 && deleted(&v, recs[i].xmax, recs[i].cmax))
			dead[ndead++] = recs[i].tid & SIFT_PD_TID_MASK;
	for (int i = 0; i < nstaged; i++)
		if (deleted(&v, st->dels[i].xmax, st->dels[i].cmax))
			dead[ndead++] = st->dels[i].tid;
	px = ndead > 0 ? sift_pdel_idx_build(dead, ndead) : NULL;
	pfree(dead);
	if (recs != NULL)
		pfree(recs);
	return px;
}

/* The snapshot sees the row at tid deleted by a pending delete of the partition or by one the transaction has not written yet: two lookups by address, none of them reads the chain again */
bool
sift_tbl_f2_pending_dead(SiftTblVis *v, const SiftPartitionEntry *entry, uint64 tid)
{
	TransactionId xmax;
	CommandId	cmax;
	bool		has_succ;
	uint64		succ;
	const SiftPendDelLog *log;

	if (v->snap->snapshot_type == SNAPSHOT_ANY)
		return false;
	if (sift_stage_find_delete(v->rel, tid, &xmax, &cmax, &has_succ, &succ) && deleted(v, xmax, cmax))
		return true;
	log = sift_pend_del_log(v->rel, entry, v->snap->curcid == v->pd_cid ? v->pd_epoch : 0);
	for (int i = log != NULL ? sift_pend_del_log_newest(log, tid) : -1; i >= 0; i = sift_pend_del_log_prev(log, i))
	{
		const SiftPendDelRec *r = sift_pend_del_log_rec(log, i);

		if (deleted(v, r->xmax, r->cmax))
			return true;
	}
	return false;
}

/* ---- granules ---- */

static bool
row_dead_in_dv(SiftTblVis *v, const SiftDvGran *dvg, uint16 row)
{
	bool		dead;
	const SiftDvXmaxEntry *e = sift_dv_find(dvg, row, &dead);

	return dead && deleted(v, e != NULL ? e->xmax : InvalidTransactionId, e != NULL ? e->cmax : 0);
}

static bool
row_inserted(SiftTblVis *v, SiftF2GranuleBuf *gb, uint16 row)
{
	if (gb->is_all_frozen)
		return true;
	if (gb->is_single_xact)
		return inserted(v, gb->single_xmin, gb->single_cmin);
	return inserted(v, gb->vers[row].xmin, gb->vers[row].cmin);
}

static bool
row_visible(SiftTblVis *v, SiftF2GranuleBuf *gb, uint16 row, const SiftPendDelIndex *pdel, bool pending_dead)
{
	const SiftDvGran *dvg = gb->pin != NULL && gb->pin->dv != NULL ? sift_dv_granule(gb->pin->dv, gb->granule_id) : NULL;

	if (v->snap->snapshot_type == SNAPSHOT_DIRTY)
		v->snap->xmin = v->snap->xmax = InvalidTransactionId;
	if (!row_inserted(v, gb, row))
		return false;
	if (dvg != NULL && row_dead_in_dv(v, dvg, row))
		return false;
	if (pending_dead || (pdel != NULL && sift_pdel_idx_has(pdel, sift_f2_gbuf_tids(gb)[row])))
		return false;
	if (v->snap->snapshot_type == SNAPSHOT_DIRTY)
		v->snap->speculativeToken = sift_tbl_rowlock_speculative_token(v->rel, sift_f2_gbuf_tids(gb)[row]);
	return true;
}

bool
sift_tbl_f2_vis_row(SiftTblVis *v, SiftF2GranuleBuf *gb, uint16 row, const SiftPendDelIndex *pdel)
{
	return row_visible(v, gb, row, pdel, false);
}

bool
sift_tbl_f2_vis_point(SiftTblVis *v, SiftF2GranuleBuf *gb, uint16 row, bool pending_dead)
{
	return row_visible(v, gb, row, NULL, pending_dead);
}

static void
select_all(uint64 *sel, uint16 nrows)
{
	memset(sel, 0, sizeof(uint64) * SIFT_DV_MASK_WORDS);
	for (uint16 r = 0; r < nrows; r++)
		sel[r / 64] |= UINT64CONST(1) << (r % 64);
}

static uint16
count_set(const uint64 *sel)
{
	uint16		n = 0;

	for (int w = 0; w < SIFT_DV_MASK_WORDS; w++)
		n += pg_popcount64(sel[w]);
	return n;
}

static bool
inserts_all_visible(SiftTblVis *v, SiftF2GranuleBuf *gb)
{
	return gb->is_all_frozen || (gb->pin != NULL && mvcc_like(v->snap) && FullTransactionIdIsValid(v->snap_xmin) && sift_vis_part_inserts_settled(&gb->pin->desc, v->snap_xmin));
}

uint16
sift_tbl_f2_vis_granule(SiftTblVis *v, SiftF2GranuleBuf *gb, const SiftPendDelIndex *pdel, uint64 *sel)
{
	const SiftDvGran *dvg = gb->pin != NULL && gb->pin->dv != NULL ? sift_dv_granule(gb->pin->dv, gb->granule_id) : NULL;
	bool		fast = v->snap->snapshot_type == SNAPSHOT_MVCC && !v->serializable;

	select_all(sel, gb->nrows);
	if (!inserts_all_visible(v, gb))
	{
		if (gb->is_single_xact && !inserted(v, gb->single_xmin, gb->single_cmin))
			return memset(sel, 0, sizeof(uint64) * SIFT_DV_MASK_WORDS), 0;
		for (uint16 r = 0; !gb->is_single_xact && r < gb->nrows; r++)
			if (!inserted(v, gb->vers[r].xmin, gb->vers[r].cmin))
				sel[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	}
	if (dvg != NULL && fast)
	{
		uint64		del[SIFT_DV_MASK_WORDS] pg_attribute_aligned(64);

		if (sift_dv_mask64(dvg, v->snap, del) > 0)
			for (int w = 0; w < SIFT_DV_MASK_WORDS; w++)
				sel[w] &= ~del[w];
	}
	else if (dvg != NULL)
		for (uint16 r = 0; r < gb->nrows; r++)
			if ((sel[r / 64] & (UINT64CONST(1) << (r % 64))) != 0 && row_dead_in_dv(v, dvg, r))
				sel[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	if (pdel != NULL && gb->pin != NULL && sift_pdel_idx_may_hit_granule(pdel, &gb->pin->tidx[gb->granule_id]))
	{
		const uint64 *tids = sift_f2_gbuf_tids(gb);

		for (uint16 r = 0; r < gb->nrows; r++)
			if ((sel[r / 64] & (UINT64CONST(1) << (r % 64))) != 0 && sift_pdel_idx_has(pdel, tids[r]))
				sel[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	}
	return count_set(sel);
}

/* ---- SERIALIZABLE ---- */

void
sift_tbl_f2_predicate_scan(Relation rel, Snapshot snap)
{
	if (IsolationIsSerializable())
		PredicateLockRelation(rel, snap);
}

void
sift_tbl_f2_predicate_point(Relation rel, uint64 tid, Snapshot snap)
{
	if (IsolationIsSerializable())
		PredicateLockPage(rel, (BlockNumber) (tid >> 16), snap);
}

void
sift_tbl_f2_conflict_in(Relation rel, uint64 tid)
{
	CheckForSerializableConflictIn(rel, NULL, (BlockNumber) (tid >> 16));
}

/* ---- BM25 ---- */

typedef struct PageFilter
{
	BlockNumber vblock;
	const uint64 *want;
	uint64	   *seen_visible;
	Snapshot	snap;
	Relation	rel;
	SiftTblVis	vis;
	SiftPendDelIndex *pdel;
	uint64		pdel_part;
} PageFilter;

/* The valid TIDs of [tid_min, tid_max] (offsets 1..SIFT_TBL_ROWS_PER_VBLOCK of each virtual block): as many as the granule has distinct rows when it holds every one of them */
static bool
holds_every_tid(const SiftTidIndexEntry *t, uint16 nrows)
{
	const uint64 vmin = t->tid_min >> 16,
				vmax = t->tid_max >> 16;
	const uint32 omin = t->tid_min & 0xFFFF,
				omax = t->tid_max & 0xFFFF;

	if (omin < 1 || omin > SIFT_TBL_ROWS_PER_VBLOCK || omax < 1 || omax > SIFT_TBL_ROWS_PER_VBLOCK || t->tid_max < t->tid_min)
		return false;
	if (vmin == vmax)
		return (uint64) omax - omin + 1 == nrows;
	return (uint64) (SIFT_TBL_ROWS_PER_VBLOCK - omin + 1) + (vmax - vmin - 1) * SIFT_TBL_ROWS_PER_VBLOCK + omax == nrows;
}

/* Sets the 1-based offsets lo..hi of the page mask */
static void
set_offsets(uint64 *mask, uint32 lo, uint32 hi)
{
	for (uint32 bit = lo - 1; bit < hi; bit++)
		mask[bit / 64] |= UINT64CONST(1) << (bit % 64);
}

/*
 * A granule whose rows every snapshot sees (settled inserts, no deletion vector entry, no pending delete among its TIDs) and that holds every TID of its range shows the page the
 * offsets of that range, whichever rows they are: no TID is decoded
 */
static bool
page_all_visible(PageFilter *f, SiftF2GranuleBuf *gb)
{
	const SiftTidIndexEntry *t;
	uint64		vmin,
				vmax;

	if (f->snap->snapshot_type != SNAPSHOT_MVCC || gb->pin == NULL || gb->pin->tidx == NULL || !inserts_all_visible(&f->vis, gb) ||
		(gb->pin->dv != NULL && sift_dv_granule(gb->pin->dv, gb->granule_id) != NULL))
		return false;
	t = &gb->pin->tidx[gb->granule_id];
	if (!holds_every_tid(t, gb->nrows) || (f->pdel != NULL && sift_pdel_idx_may_hit_granule(f->pdel, t)))
		return false;
	vmin = t->tid_min >> 16;
	vmax = t->tid_max >> 16;
	if (f->vblock >= vmin && f->vblock <= vmax)
		set_offsets(f->seen_visible, f->vblock == vmin ? t->tid_min & 0xFFFF : 1, f->vblock == vmax ? t->tid_max & 0xFFFF : SIFT_TBL_ROWS_PER_VBLOCK);
	return true;
}

static bool
filter_granule(SiftF2GranuleBuf *gb, const SiftPartitionEntry *entry, void *arg)
{
	PageFilter *f = arg;
	const uint64 *tids;

	if (f->pdel == NULL || f->pdel_part != entry->part_id)
	{
		sift_pdel_idx_free(f->pdel);
		f->pdel = sift_tbl_f2_pending_index(f->rel, entry, f->snap);
		f->pdel_part = entry->part_id;
	}
	if (page_all_visible(f, gb))
		return false;
	tids = sift_f2_gbuf_tids(gb);
	for (uint16 r = 0; r < gb->nrows; r++)
	{
		OffsetNumber off = tids[r] & 0xFFFF;
		uint32		bit = off - 1;

		if ((tids[r] >> 16) != f->vblock || off == 0 || off > SIFT_TBL_ROWS_PER_VBLOCK || (f->want[bit / 64] & (UINT64CONST(1) << (bit % 64))) == 0)
			continue;
		if (sift_tbl_f2_vis_row(&f->vis, gb, r, f->pdel))
			f->seen_visible[bit / 64] |= UINT64CONST(1) << (bit % 64);
	}
	return false;
}

void
sift_tbl_f2_filter_page(Relation rel, Snapshot snap, BlockNumber vblock, uint64 *offmask)
{
	uint64		visible[SIFT_TBL_OFFMASK_WORDS] = {0};
	PageFilter	f = {.vblock = vblock,.want = offmask,.seen_visible = visible,.snap = snap,.rel = rel};

	sift_stage_flush(rel);
	sift_tbl_f2_vis_begin(&f.vis, rel, snap);
	sift_tbl_f2_predicate_point(rel, (uint64) vblock << 16, snap);
	sift_f2_visit_granules(rel, sift_f2_store_open(rel), (uint64) vblock << 16, ((uint64) vblock << 16) | 0xFFFF, filter_granule, &f);
	for (int w = 0; w < SIFT_TBL_OFFMASK_WORDS; w++)
		offmask[w] &= visible[w];
}

bool
sift_tbl_f2_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	SiftF2Loc loc;
	SiftTblVis	v;
	bool		ok;
	SiftF2Store *store = sift_f2_store_open(rel);
	uint64		tid = sift_tbl_tid_pack(&slot->tts_tid);
	bool		waiting;

	sift_stage_flush(rel);
	if (!sift_tbl_f2_locate_or_wait(rel, store, tid, snapshot, &loc, &waiting))
		return waiting;
	sift_tbl_f2_vis_begin(&v, rel, snapshot);
	ok = sift_tbl_f2_vis_point(&v, loc.gb, loc.row, sift_tbl_f2_pending_dead(&v, &loc.entry, tid));
	sift_f2_gbuf_unref(loc.gb);
	if (ok)
		sift_tbl_f2_predicate_point(rel, tid, snapshot);
	return ok;
}
