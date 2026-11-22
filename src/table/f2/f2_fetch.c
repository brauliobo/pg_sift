/*
 * table/f2/f2_fetch.c - reading rows of a sift_table by address: the index fetch, and the fetch of a row version, which a transaction keeps one of for all its rows.
 *
 * The granule of the last row stays with the fetch, so that the rows of one granule in a row of calls cost one lookup; what the snapshot sees of the pending deletes and the patches of
 * column deltas is read again for each call that is not of SnapshotAny, the stage of the transaction being part of it.
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/col_meta.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_gcache.h"
#include "table/f2/f2_slot.h"
#include "table/f2/f2_tbl.h"

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
	/* MEM[MEM-TBL-11]: query-scoped f2 TID fetch ctx + rowcx (ALLOCSET_SMALL_SIZES) */
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
	const SiftColDeltaGranMap *gm;
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
	gm = sift_col_delta_scan_map(f->delta, &f->entry, f->gb);
	sift_f2_slot_store(slot, rel, f->gb, (uint16) row, sift_col_delta_scan_part_map(f->delta), gm, f->rowcx);
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

/* ---- the fetch of the transaction ---- */

static SiftTblF2Fetch *tx_fetch = NULL;
static Oid	tx_relid = InvalidOid;
static uint64 tx_manifest_version = 0;
static bool tx_callback_registered = false;

/* The transaction ends: the fetch gives its granule back; its contexts go with TopTransactionContext */
static void
tx_fetch_xact_callback(XactEvent event, void *arg pg_attribute_unused())
{
	if (tx_fetch == NULL || event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PARALLEL_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
		return;
	fetch_drop_granule(tx_fetch);
	tx_fetch = NULL;
}

/* The fetch for rows of the relation: kept while the relation, its store and its manifest are the ones it was made for, so that a transaction that fetches a row after row of it pays for the fetch once */
static SiftTblF2Fetch *
tx_fetch_for(Relation rel)
{
	SiftTblF2Fetch *f = tx_fetch;
	uint64		version = sift_col_meta_read(rel).manifest_version;
	SiftF2Store *store = sift_f2_store_open(rel);

	if (f == NULL || f->base.rel != rel || tx_relid != RelationGetRelid(rel) || tx_manifest_version != version || f->store != store)
	{
		if (f != NULL)
		{
			fetch_drop_granule(f);
			MemoryContextDelete(f->cx);
		}
		f = tx_fetch = fetch_new(rel, AllocSetContextCreate(TopTransactionContext, "sift f2 transaction fetch", ALLOCSET_SMALL_SIZES));
		tx_relid = RelationGetRelid(rel);
		tx_manifest_version = version;
		if (!tx_callback_registered)
		{
			RegisterXactCallback(tx_fetch_xact_callback, NULL);
			tx_callback_registered = true;
		}
	}
	return f;
}

bool
sift_tbl_f2_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	SiftTblF2Fetch *f = tx_fetch_for(rel);
	bool		found;

	f->pdel_valid = false;
	sift_col_delta_scan_reset(f->delta);
	found = fetch_one(f, sift_tbl_tid_pack(tid), snapshot, slot);
	if (found)
		ExecMaterializeSlot(slot);
	return found;
}
