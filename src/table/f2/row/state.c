/*
 * table/f2/row/state.c - the row on disk and in the stage of the transaction (f2_row_state.h).
 *
 * The deleter of a row is in its partition's delete vector, in the pending deletes that follow it (the pair of the old address and its successor of an update), or, for the transaction's own
 * deletes not yet written, in its stage: a row the transaction deleted is known to it whether or not a lock entry survived the escalation of its locks.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"

#include "col/f2/meta.h"
#include "col/f2/l0/stage.h"
#include "table/f2/chain_cache.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/lock/rowlock.h"
#include "table/f2/row/state.h"
#include "table/f2/gcache.h"
#include "table/f2/tbl.h"
#include "core/compat.h"

static bool
newer_deleter(TransactionId xmax)
{
	return TransactionIdIsValid(xmax) && !TransactionIdDidAbort(xmax);
}

static void
pending_state(Relation rel, const SiftPartitionEntry *entry, uint64 tid, SiftRowState *rs)
{
	const SiftPendDelLog *log = sift_pend_del_log(rel, entry, 0);

	for (int i = log == NULL ? -1 : sift_pend_del_log_newest(log, tid); i >= 0; i = sift_pend_del_log_prev(log, i))
	{
		const SiftPendDelRec *r = sift_pend_del_log_rec(log, i);
		bool		paired = (r->tid & SIFT_PD_TID_UPDATED) != 0 && i + 1 < sift_pend_del_log_count(log) && (sift_pend_del_log_rec(log, i + 1)->tid & SIFT_PD_TID_SUCC) != 0;

		if (!newer_deleter(r->xmax))
			continue;
		rs->xmax = r->xmax;
		rs->cmax = r->cmax;
		rs->has_succ = paired;
		rs->succ = paired ? sift_pend_del_log_rec(log, i + 1)->tid & SIFT_PD_TID_MASK : 0;
		return;
	}
}

static void
vector_state(SiftF2GranuleBuf *gb, uint16 row, SiftRowState *rs)
{
	const SiftDvGran *dvg = gb->pin->dv != NULL ? sift_dv_granule(gb->pin->dv, gb->granule_id) : NULL;
	bool		dead;
	const SiftDvXmaxEntry *e;

	if (dvg == NULL)
		return;
	e = sift_dv_find(dvg, row, &dead);
	if (dead && (e == NULL || newer_deleter(e->xmax)))
	{
		rs->xmax = e != NULL ? e->xmax : FrozenTransactionId;
		rs->cmax = e != NULL ? e->cmax : 0;
	}
}

static void
stage_state(Relation rel, uint64 tid, SiftRowState *rs)
{
	TransactionId xmax;
	CommandId	cmax;
	bool		has_succ;
	uint64		succ;

	if (!sift_stage_find_delete(rel, tid, &xmax, &cmax, &has_succ, &succ))
		return;
	rs->xmax = xmax;
	rs->cmax = cmax;
	rs->has_succ = has_succ;
	rs->succ = succ;
}

static SiftRowState
row_state(Relation rel, uint64 tid, bool (*locate) (Relation, SiftF2Store *, uint64, SiftF2Loc *))
{
	SiftRowState rs = {.xmax = InvalidTransactionId};
	SiftF2Loc	loc;
	SiftF2GranuleBuf *gb;

	if (!locate(rel, sift_f2_store_open(rel), tid, &loc))
		return rs;
	gb = loc.gb;
	rs.found = true;
	rs.part_id = loc.entry.part_id;
	rs.xmin = gb->is_all_frozen ? FrozenTransactionId : gb->is_single_xact ? gb->single_xmin : gb->vers[loc.row].xmin;
	rs.cmin = gb->is_single_xact ? gb->single_cmin : gb->is_all_frozen ? 0 : gb->vers[loc.row].cmin;
	vector_state(gb, loc.row, &rs);
	pending_state(rel, &loc.entry, tid, &rs);
	stage_state(rel, tid, &rs);
	sift_f2_gbuf_unref(gb);
	return rs;
}

SiftRowState
sift_row_state_read(Relation rel, uint64 tid)
{
	return row_state(rel, tid, sift_f2_locate_cached_read);
}

/* A row of a micro-slab read where it is, from the granules as they are on disk */
static bool
locate_in_place(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	return sift_f2_locate_read(rel, store, tid, sift_f2_gbuf_load, loc);
}

SiftRowState
sift_row_state_read_settled(Relation rel, uint64 tid)
{
	return row_state(rel, tid, locate_in_place);
}

/* The version of the row a reader holds: every one its own transaction wrote (the new row an UPDATE returns included), else what its snapshot sees */
static bool
reader_sees(void *ctx, TransactionId xmin, CommandId cid)
{
	return TransactionIdIsCurrentTransactionId(xmin) || sift_tbl_f2_delta_visible((SiftTblVis *) ctx, xmin, cid);
}

/* The writers of the column deltas of a row of a part, as the chain of its partition says and the reader sees them */
static SiftColDeltaWriters
deltas_on_disk(Relation rel, uint64 tid, const SiftColDeltaOps *ops)
{
	SiftF2Loc	loc;
	SiftColDeltaWriters w = {0};

	if (!sift_f2_locate_cached_read(rel, sift_f2_store_open(rel), tid, &loc))
		return w;
	w = sift_col_delta_writers_of(rel, &loc, tid, ops);
	sift_f2_gbuf_unref(loc.gb);
	return w;
}

/* The writers of the column deltas of the row: the one whose entry of the row lock has not committed is the newest, wherever its record is */
static SiftColDeltaWriters
delta_writers(Relation rel, uint64 tid, Snapshot snap, const SiftRowLockPeek *held)
{
	SiftTblVis	vis;
	SiftColDeltaOps ops;
	SiftColDeltaWriters w;
	SiftColDeltaLast pending = {.found = TransactionIdIsValid(held->delta_xid),.xid = held->delta_xid,.cid = held->delta_cid};

	sift_tbl_f2_vis_begin(&vis, rel, snap);
	sift_col_delta_ops_make(&ops, &vis, reader_sees);
	w = deltas_on_disk(rel, tid, &ops);
	if (!pending.found)
		return w;
	w.newest = pending;
	if (reader_sees(&vis, pending.xid, pending.cid))
		w.seen = pending;
	return w;
}

/*
 * A column delta is a new version of the row in place: the snapshot sees the version of the newest delta it sees (the insert without one), and a newer delta that it does not see is the
 * update that ended that version, as the xmax of the old tuple of a heap UPDATE is
 */
SiftRowState
sift_row_sysattr_read(Relation rel, uint64 tid, Snapshot snap)
{
	SiftRowState rs = sift_row_state_read(rel, tid);
	SiftRowLockPeek held = sift_tbl_rowlock_peek(rel, tid);
	SiftColDeltaWriters w = {0};

	if (!rs.found)
	{
		rs.found = sift_stage_find_insert(rel, tid, &rs.xmin, &rs.cmin);
		stage_state(rel, tid, &rs);
	}
	else
		w = delta_writers(rel, tid, snap, &held);
	if (w.seen.found)
	{
		rs.xmin = w.seen.xid;
		rs.cmin = w.seen.cid;
	}
	if (TransactionIdIsValid(rs.xmax))
		return rs;
	if (!w.newest.found)
		rs.xmax = held.locker;
	else if (!w.seen.found || w.seen.xid != w.newest.xid || w.seen.cid != w.newest.cid)
	{
		rs.xmax = w.newest.xid;
		rs.cmax = w.newest.cid;
	}
	return rs;
}

SiftRowState
sift_row_version_read(Relation rel, uint64 tid, Snapshot snap)
{
	SiftRowState rs = sift_row_state_read(rel, tid);
	SiftTblVis	vis;
	SiftColDeltaOps ops;
	SiftColDeltaWriters w;

	sift_tbl_f2_vis_begin(&vis, rel, snap);
	sift_col_delta_ops_init(&ops, &vis);
	w = deltas_on_disk(rel, tid, &ops);
	if (w.seen.found)
	{
		rs.xmin = w.seen.xid;
		rs.cmin = w.seen.cid;
	}
	return rs;
}

void
sift_tbl_f2_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	Relation	rel = scan->rs_rd;
	uint64		cur = sift_tbl_tid_pack(tid),
				succ;

	sift_stage_flush(rel);
	while (sift_row_visible_successor(rel, cur, scan->rs_snapshot, &succ) && succ != SIFT_MOVED_PARTITIONS_TID)
		cur = succ;
	sift_tbl_tid_unpack(tid, cur);
}

TM_Result
sift_row_state_result(const SiftRowState *rs, CommandId cid, TM_FailureData *tmfd, uint64 tid)
{
	if (!rs->found)
		return TM_Deleted;
	if (TransactionIdIsNormal(rs->xmin) &&
		(TransactionIdIsCurrentTransactionId(rs->xmin) ? rs->cmin >= cid : (TransactionIdIsInProgress(rs->xmin) || !TransactionIdDidCommit(rs->xmin))))
		return TM_Invisible;
	if (!TransactionIdIsValid(rs->xmax))
		return TM_Ok;
	tmfd->xmax = rs->xmax;
	tmfd->cmax = rs->cmax;
	if (TransactionIdIsCurrentTransactionId(rs->xmax))
		return rs->cmax >= cid ? TM_SelfModified : TM_Invisible;
	if (TransactionIdIsInProgress(rs->xmax))
		return TM_BeingModified;
	if (rs->has_succ)
	{
		sift_tbl_tid_unpack(&tmfd->ctid, rs->succ);
		return TM_Updated;
	}
	sift_tbl_tid_unpack(&tmfd->ctid, tid);
	return TM_Deleted;
}

bool
sift_row_visible_successor(Relation rel, uint64 tid, Snapshot snap, uint64 *succ)
{
	SiftRowState rs = sift_row_state_read(rel, tid);

	*succ = rs.succ;
	return rs.found && rs.has_succ && sift_vis_deleted(rs.xmax, rs.cmax, snap);
}

TM_Result
sift_row_delta_result(Relation rel, uint64 tid, CommandId cid, Snapshot snap, TM_FailureData *tmfd)
{
	SiftF2Loc	loc;
	SiftColDeltaVerdict v = {.result = TM_Ok};

	if (!sift_f2_locate_cached_read(rel, sift_f2_store_open(rel), tid, &loc))
		return TM_Ok;
	if ((loc.entry.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		v = sift_col_delta_modify_verdict(rel, &loc, tid, cid, snap);
	sift_f2_gbuf_unref(loc.gb);
	if (v.result != TM_Ok)
	{
		tmfd->xmax = v.xmax;
		tmfd->cmax = v.cmax;
		sift_tbl_tid_unpack(&tmfd->ctid, tid);
	}
	return v.result;
}

void
sift_row_delta_ack(Relation rel, uint64 tid, CommandId cid)
{
	SiftF2Loc	loc;

	if (!sift_f2_locate_cached_read(rel, sift_f2_store_open(rel), tid, &loc))
		return;
	if ((loc.entry.flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
		sift_col_delta_ack_latest(rel, &loc, tid, cid);
	sift_f2_gbuf_unref(loc.gb);
}

uint32
sift_row_part_count(Relation rel)
{
	return sift_col_meta_read(rel).npartitions;
}

bool
sift_row_dead_to_all(Relation rel, GlobalVisState *vistest, uint64 tid, TransactionId *xmax)
{
	SiftRowState rs = sift_row_state_read_settled(rel, tid);

	*xmax = rs.xmax;
	return rs.found && TransactionIdIsValid(rs.xmax) && !TransactionIdIsInProgress(rs.xmax) && (!TransactionIdIsNormal(rs.xmax) || TransactionIdDidCommit(rs.xmax)) &&
		sift_vis_removable_xid(vistest, rs.xmax);
}

/*
 * Only a row on disk whose deleter committed before every snapshot is known dead: a row no part holds may be staged by a running transaction of any backend (its entry is in the index before
 * the row is written), and the orphan an aborted one left is VACUUM's to reap (table/f2/vm). A row the transaction staged itself is alive and is not read, as a read of it writes the stage out;
 * the others are read where they are: nothing is written to ask the relation about a row, which at every index page the btree tries to clean made a part of the stage or of a micro-slab.
 *
 * What the btree reads back (access/tableam.h): an entry its caller marked deletable stays deletable, and the array ends at the last deletable entry, so that no deletable entry at all is
 * an empty array, which only a bottom-up caller may get.
 */
TransactionId
sift_tbl_f2_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	GlobalVisState *vistest = GlobalVisTestFor(rel);
	TransactionId horizon = InvalidTransactionId;
	int			final_ndeltids = 0;

	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexStatus *status = &delstate->status[delstate->deltids[i].id];
		uint64		tid = sift_tbl_tid_pack(&delstate->deltids[i].tid);
		TransactionId xmax = InvalidTransactionId;
		bool		deletable = !sift_stage_holds_in(rel, tid, tid) && sift_row_dead_to_all(rel, vistest, tid, &xmax);

		status->knowndeletable |= deletable;
		if (deletable && TransactionIdIsNormal(xmax) && (!TransactionIdIsValid(horizon) || TransactionIdFollows(xmax, horizon)))
			horizon = xmax;
		if (status->knowndeletable)
			final_ndeltids = i + 1;
	}
	delstate->ndeltids = final_ndeltids;
	return horizon;
}
