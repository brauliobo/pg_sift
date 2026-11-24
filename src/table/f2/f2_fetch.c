/*
 * table/f2/f2_fetch.c - reading rows of a sift_table by address: the index fetch, and the fetch of a row version, which a transaction keeps one of for all its rows.
 *
 * The granule of the last row stays with the fetch, so that the rows of one granule in a row of calls cost one lookup; what the snapshot sees of the pending deletes (the stage of the
 * transaction being part of them) is asked of the row, by its address, in each call that is not of SnapshotAny: a fetch holds no index of them, so a probe costs the same at the first
 * delete of a transaction and at the last. The patches of column deltas are read again for each call.
 *
 * A row of a part that has a row pack (f2_row_pack.h) is found by the address hash of the part and read from its block, with no granule: when its granule needs no version (frozen, settled for the
 * snapshot, or the rows of one transaction) and holds no deleted row. A part with a column delta reads the granule too, for the map of the patches. Any other row is read from its granule.
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/col_meta.h"
#include "col/f2/part_vis.h"
#include "core/damage.h"
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

typedef struct PackedFetch
{
	SiftTblF2Fetch *f;
	Snapshot	snapshot;
	TupleTableSlot *slot;
	uint64		tid;
	bool		found;			/* the row is in the slot */
} PackedFetch;

/* Whether the snapshot sees the insert of the rows of the granule without a version of each: a frozen granule, a part whose inserters are all settled, or the rows of one transaction */
static bool
packed_inserted(SiftTblF2Fetch *f, const SiftF2PartPin *pin, const SiftGranuleDesc *gd)
{
	TransactionId xmin = XidFromFullTransactionId(gd->min_xid);

	if ((gd->flags & SIFT_GDESC_ALL_FROZEN) || (FullTransactionIdIsValid(f->vis.snap_xmin) && sift_vis_part_inserts_settled(&pin->desc, f->vis.snap_xmin)))
		return true;
	return (gd->flags & SIFT_GDESC_SINGLE_XACT) && (!TransactionIdIsNormal(xmin) || sift_vis_inserted_cached(&f->vis.xc, xmin, gd->single_cmin));
}

/* Whether the row is there for the snapshot, as sift_tbl_f2_vis_row decides it for a row of a granule that has no versions of its own */
static bool
packed_visible(SiftTblF2Fetch *f, const SiftF2PartPin *pin, const SiftPartitionEntry *entry, uint32 granule_id, uint64 tid, Snapshot snapshot)
{
	if (!packed_inserted(f, pin, &pin->gdir[granule_id]) || (pin->dv != NULL && sift_dv_granule(pin->dv, granule_id) != NULL))
		return false;
	f->entry = *entry;
	return !sift_tbl_f2_pending_dead(&f->vis, entry, tid);
}

static void
packed_store(PackedFetch *p, SiftF2PartPin *pin, const SiftPartitionEntry *entry, const SiftTidHashHit *hit)
{
	SiftTblF2Fetch *f = p->f;
	Relation	rel = f->base.rel;
	SiftF2GranuleBuf *gb = NULL;
	const SiftColDeltaGranMap *gm = NULL;

	sift_tbl_f2_predicate_point(rel, p->tid, p->snapshot);
	if (!packed_visible(f, pin, entry, hit->granule_id, p->tid, p->snapshot))
		return;
	if (entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA)
	{
		gb = sift_f2_gcache_get(rel, f->store, pin, hit->granule_id);
		gm = sift_col_delta_scan_map(f->delta, entry, gb);
	}
	sift_f2_slot_store_packed(p->slot, rel, pin, hit->granule_id, (uint16) (hit->row_idx - pin->gdir[hit->granule_id].row_base), sift_f2_gcache_row_pack_block(rel, pin, hit->row_idx / SIFT_ROW_PACK_ROWS),
							  hit->row_idx % SIFT_ROW_PACK_ROWS, p->tid, gb, sift_col_delta_scan_part_map(f->delta), gm, f->rowcx);
	if (gb != NULL)
		sift_f2_gbuf_unref(gb);
	p->found = true;
}

/* The row of the address among the rows the hash names: true when it is one of them, whether or not the snapshot sees it (the granule path decides that again) */
static bool
packed_hit(SiftF2PartPin *pin, const SiftPartitionEntry *entry, const SiftTidHashHit *hits, int nhits, void *arg)
{
	PackedFetch *p = arg;
	Relation	rel = p->f->base.rel;

	for (int i = 0; i < nhits; i++)
	{
		const SiftTidHashHit *h = &hits[i];
		const SiftRowPackBlk *blk;

		blk = sift_f2_gcache_row_pack_block(rel, pin, h->row_idx / SIFT_ROW_PACK_ROWS);
		if (sift_row_pack_holds(blk, h->row_idx % SIFT_ROW_PACK_ROWS, p->tid))
		{
			packed_store(p, pin, entry, h);
			return true;
		}
	}
	return false;
}

static bool
fetch_packed(SiftTblF2Fetch *f, uint64 tid, Snapshot snapshot, TupleTableSlot *slot)
{
	PackedFetch p = {.f = f,.snapshot = snapshot,.slot = slot,.tid = tid};

	f->store = sift_f2_store_open(f->base.rel);
	return sift_f2_locate_packed(f->base.rel, f->store, tid, packed_hit, &p) && p.found;
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
		sift_col_delta_scan_reset(f->delta);
	}
	row = f->gb != NULL ? row_in_granule(f->gb, tid) : -1;
	ExecClearTuple(slot);
	MemoryContextReset(f->rowcx);
	if (row < 0)
	{
		SiftF2Loc	loc;
		bool		waiting;

		if (snapshot->snapshot_type == SNAPSHOT_MVCC && fetch_packed(f, tid, snapshot, slot))
			return true;
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
	sift_tbl_f2_predicate_point(rel, tid, snapshot);
	if (!sift_tbl_f2_vis_point(&f->vis, f->gb, (uint16) row, sift_tbl_f2_pending_dead(&f->vis, &f->entry, tid)))
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

uint64
sift_tbl_f2_fetch_order(struct IndexFetchTableData *scan, const ItemPointerData *tid)
{
	return sift_f2_locate_order(scan->rel, sift_f2_store_open(scan->rel), sift_tbl_tid_pack(tid));
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

	sift_col_delta_scan_reset(f->delta);
	found = fetch_one(f, sift_tbl_tid_pack(tid), snapshot, slot);
	if (found)
		ExecMaterializeSlot(slot);
	return found;
}
