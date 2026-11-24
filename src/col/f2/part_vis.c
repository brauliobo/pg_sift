/*
 * col/part_vis.c - visibility in format major 2 (col/part_vis.h).
 */
#include "core/host.h"

#include "access/transam.h"
#include "access/xact.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"

#include "col/f2/part_vis.h"
#include "core/page_io.h"

FullTransactionId
sift_vis_snap_xmin(Snapshot snap)
{
	if (snap == NULL || snap->snapshot_type != SNAPSHOT_MVCC)
		return InvalidFullTransactionId;
	return sift_xid_fxid_at(ReadNextFullTransactionId(), snap->xmin);
}

static inline bool
older(FullTransactionId a, FullTransactionId snap_xmin)
{
	return !FullTransactionIdIsValid(a) || FullTransactionIdPrecedes(a, snap_xmin);
}

bool
sift_vis_part_inserts_settled(const SiftPartDesc *d, FullTransactionId snap_xmin)
{
	return FullTransactionIdIsValid(snap_xmin) && (d->flags & SIFT_PART_FLAG_ALL_VISIBLE) && older(d->max_xid, snap_xmin);
}

static inline bool
part_dv_settled(const SiftPartDesc *d, FullTransactionId snap_xmin)
{
	return (d->flags & SIFT_PART_FLAG_DV_SETTLED) && older(d->dv_max_xid, snap_xmin);
}

SiftVisClass
sift_vis_part_class(const SiftPartDesc *d, bool has_pend_del, FullTransactionId snap_xmin)
{
	if (has_pend_del || !sift_vis_part_inserts_settled(d, snap_xmin))
		return SIFT_VIS_PER_ROW;
	if (d->dv_blk == InvalidBlockNumber)
		return SIFT_VIS_PART_CLEAN;
	return part_dv_settled(d, snap_xmin) ? SIFT_VIS_PART_SETTLED_DV : SIFT_VIS_PER_ROW;
}

static inline bool
granule_inserts_settled(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId snap_xmin)
{
	return FullTransactionIdIsValid(snap_xmin) && ((g->flags & SIFT_GDESC_ALL_FROZEN) || ((d->flags & SIFT_PART_FLAG_ALL_VISIBLE) && older(g->max_xid, snap_xmin)));
}

bool
sift_vis_granule_inserts_settled(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId snap_xmin)
{
	return granule_inserts_settled(d, g, snap_xmin);
}

/* The dead rows of the granule are dead outright: it keeps no deleter, or only deleters that committed before the snapshot */
static inline bool
granule_dv_settled(const SiftPartDesc *d, FullTransactionId dv_xid_max, FullTransactionId snap_xmin)
{
	return !FullTransactionIdIsValid(dv_xid_max) || ((d->flags & SIFT_PART_FLAG_DV_SETTLED) && older(dv_xid_max, snap_xmin));
}

SiftVisClass
sift_vis_granule_class(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId dv_xid_max, bool pend_hit, FullTransactionId snap_xmin)
{
	if (pend_hit || !granule_inserts_settled(d, g, snap_xmin) || !granule_dv_settled(d, dv_xid_max, snap_xmin))
		return SIFT_VIS_PER_ROW;
	return SIFT_VIS_GRANULE_SETTLED;
}

bool
sift_vis_inserted(uint32 xmin, uint32 cmin, Snapshot snap)
{
	if (!TransactionIdIsNormal(xmin))
		return true;
	if (snap->snapshot_type != SNAPSHOT_MVCC)
		return TransactionIdIsInProgress(xmin) || TransactionIdDidCommit(xmin);
	if (TransactionIdIsCurrentTransactionId(xmin))
		return cmin < snap->curcid;
	if (XidInMVCCSnapshot(xmin, snap))
		return false;
	return TransactionIdDidCommit(xmin);
}

bool
sift_vis_deleted(TransactionId xmax, CommandId cmax, Snapshot snap)
{
	if (!TransactionIdIsValid(xmax))
		return true;
	if (snap->snapshot_type != SNAPSHOT_MVCC)
		return !TransactionIdIsInProgress(xmax) && TransactionIdDidCommit(xmax);
	if (TransactionIdIsCurrentTransactionId(xmax))
		return cmax < snap->curcid;
	if (XidInMVCCSnapshot(xmax, snap))
		return false;
	return TransactionIdDidCommit(xmax);
}

bool
sift_vis_inserted_cached(SiftXidCache *c, uint32 xmin, uint32 cmin)
{
	SiftXidStatus st;

	if (!TransactionIdIsNormal(xmin))
		return true;
	if (!c->is_mvcc)
		return sift_vis_inserted(xmin, cmin, c->snap);
	st = sift_xid_cache_status(c, xmin);
	return st == SIFT_XSTAT_COMMITTED || (st == SIFT_XSTAT_SELF && cmin < c->snap->curcid);
}

bool
sift_vis_deleted_cached(SiftXidCache *c, TransactionId xmax, CommandId cmax)
{
	SiftXidStatus st;

	if (!TransactionIdIsValid(xmax))
		return true;
	if (!c->is_mvcc)
		return sift_vis_deleted(xmax, cmax, c->snap);
	st = sift_xid_cache_status(c, xmax);
	return st == SIFT_XSTAT_COMMITTED || (st == SIFT_XSTAT_SELF && cmax < c->snap->curcid);
}

bool
sift_part_try_seal(Relation rel, uint32 part_id, SiftPartDesc *d, SiftXidHorizonCache *hc)
{
	SiftPart   *part;
	SiftGranuleDesc *dir;

	if (!(d->flags & SIFT_PART_FLAG_UNFROZEN) || (d->flags & SIFT_PART_FLAG_ALL_VISIBLE) || !older(d->max_xid, hc->horizon))
		return false;
	part = sift_part_open_desc(rel, d, part_id);
	dir = sift_part_granule_dir(rel, part);
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		SiftRowVer *vers = sift_part_read_vers(rel, part, dir, g);

		for (int r = 0; vers != NULL && r < dir[g].nrows; r++)
		{
			SiftHorizonStatus st;

			if (!TransactionIdIsNormal(vers[r].xmin))
				continue;
			st = sift_xid_horizon_status(hc, vers[r].xmin);
			if (st != SIFT_H_COMMITTED_OLD && st != SIFT_H_COMMITTED_NEW)
				return false;
		}
	}
	d->flags |= SIFT_PART_FLAG_ALL_VISIBLE;
	return true;
}

/* The addresses of the pending deletes the snapshot sees; NULL when none */
static SiftPendDelIndex *
pend_index(SiftXidCache *xc, const SiftPendDelRec *pend, int npend)
{
	uint64	   *seen = palloc(Max(npend, 1) * sizeof(uint64));
	int			n = 0;

	for (int i = 0; i < npend; i++)
		if ((pend[i].tid & SIFT_PD_TID_SUCC) == 0 && sift_vis_deleted_cached(xc, pend[i].xmax, pend[i].cmax))
			seen[n++] = pend[i].tid & SIFT_PD_TID_MASK;
	return n > 0 ? sift_pdel_idx_build(seen, n) : NULL;
}

uint64
sift_part_count_visible(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPendDelRec *pend, int npend, Snapshot snap, SiftVisStats *st)
{
	FullTransactionId sx = sift_vis_snap_xmin(snap);
	SiftVisClass pc = sift_vis_part_class(d, npend > 0, sx);
	SiftXidCache xc;
	SiftPendDelIndex *px;
	SiftTidIndexEntry *tix = NULL;
	SiftPart   *part;
	SiftGranuleDesc *dir;
	uint16	   *grows;
	SiftDv	   *dv = NULL;
	uint64		count = 0;

	if (pc == SIFT_VIS_PART_CLEAN)
	{
		st->parts_clean++;
		return d->nrows;
	}
	sift_xid_cache_init(&xc, snap);
	px = pend_index(&xc, pend, npend);
	part = sift_part_open_desc(rel, d, part_id);
	dir = sift_part_granule_dir(rel, part);
	grows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
		grows[g] = dir[g].nrows;
	if (px != NULL)
	{
		uint64		len;

		tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &len);
		if (tix == NULL || len != (uint64) part->hdr.ngranules * sizeof(SiftTidIndexEntry))
			SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "The part has no usable TID index.");
	}
	if (d->dv_blk != InvalidBlockNumber)
		dv = sift_dv_read(rel, d, part_id, grows);
	if (pc == SIFT_VIS_PART_SETTLED_DV)
		st->parts_settled_dv++;
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
		SiftDvCursor dc;
		bool		hit = px != NULL && sift_pdel_idx_may_hit_granule(px, &tix[g]);
		FullTransactionId dv_xid = dg != NULL ? dg->xid_max : InvalidFullTransactionId;
		SiftVisClass gc = pc == SIFT_VIS_PART_SETTLED_DV ? pc : sift_vis_granule_class(d, &dir[g], dv_xid, hit, sx);
		bool		dv_outright = pc == SIFT_VIS_PART_SETTLED_DV || granule_dv_settled(d, dv_xid, sx);
		SiftRowVer *vers = NULL;
		uint64	   *tids = NULL;
		int			n = dir[g].nrows;

		if (gc == SIFT_VIS_GRANULE_SETTLED)
			st->granules_settled++;
		else if (gc == SIFT_VIS_PER_ROW)
			st->granules_per_row++;
		if (!granule_inserts_settled(d, &dir[g], sx))
			vers = sift_part_read_vers(rel, part, dir, g);
		if (hit)
			tids = sift_part_read_tids(rel, part, dir, g);
		sift_dv_cursor_init(&dc, dg);
		for (int r = 0; r < n; r++)
		{
			bool		dead = false;
			const SiftDvXmaxEntry *x = sift_dv_cursor_find(&dc, r, &dead);

			if (gc == SIFT_VIS_PER_ROW)
				st->rows_checked++;
			if (vers != NULL && !sift_vis_inserted_cached(&xc, vers[r].xmin, vers[r].cmin))
				continue;
			if (dead && (dv_outright || sift_vis_deleted_cached(&xc, x->xmax, x->cmax)))
				continue;
			if (tids != NULL && sift_pdel_idx_has(px, tids[r]))
				continue;
			count++;
		}
	}
	return count;
}
