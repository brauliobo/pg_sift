/*
 * table/net.c - the net pre-aggregates of the dirty units of the column segments of a sift_table (col/seg/net.c, docs/COL-PREAGG.md section 13), written by VACUUM
 * as col/maint/net.c writes them for an index: after the liveness bits of the dead rows are cleared and the merges that rewrite a segment are done, the segments
 * that kept a dead row get the cube, the rollups and the sums of their live rows after their own stream, and the directory entry says SIFT_SEG_NET.
 * The pages go in the table's own file; a crash before the switch leaves them unreferenced (the orphan sweep returns them).
 */
#include "postgres.h"

#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "bm25/maint.h"
#include "col/am.h"
#include "col/seg.h"
#include "core/ops.h"
#include "vacuum_int.h"

/* The segment's own stream, then the sidecar's pages; the old sidecar's are released with the old header page */
static uint64
net_write(Relation rel, const SiftSegEntry *entry)
{
	SiftWriteTarget *wt = sift_wt_begin(rel, SIFT_WRITE_SHARED);
	SiftColSegment *seg = sift_cseg_open(rel, entry, CurrentMemoryContext);
	const SiftSegHeaderData *old = seg->hdr;
	uint64		at;
	uint32		keep_pages = seg->sr.npages - sift_cseg_net_pages(seg, &at);
	StringInfo	net = sift_cseg_net_build(seg);
	SiftStreamWriter *sw;
	SiftExtent *add,
			   *list,
			   *release;
	int			nadd,
				nlist = 0,
				nrelease = 0;
	SiftSegEntry fresh = *entry;
	SiftMaintSwitch sw_dir = {.old = &entry->gen,.nold = 1,.neu = &fresh};
	uint32		left = keep_pages;
	uint64		blocks = 0;

	sift_cseg_net_finish(net, at);
	sw = sift_sw_begin(wt, entry->gen);
	sift_sw_write(sw, net->data, net->len);
	add = sift_sw_finish(sw, &nadd);
	list = palloc((old->nextents + nadd) * sizeof(SiftExtent));
	release = palloc((old->nextents + 1) * sizeof(SiftExtent));
	for (uint32 e = 0; e < old->nextents; e++)
	{
		uint32		n = Min(old->extents[e].nblocks, left);

		left -= n;
		if (n > 0)
			list[nlist++] = (SiftExtent) {old->extents[e].start, n};
		if (n < old->extents[e].nblocks)
			release[nrelease++] = (SiftExtent) {old->extents[e].start + n, old->extents[e].nblocks - n};
	}
	for (int i = 0; i < nadd; i++)
	{
		list[nlist++] = add[i];
		blocks += add[i].nblocks;
	}
	if (nlist > (int) SIFT_MAX_EXTENTS)
	{
		/* the segment is written in too many extents for the header to list the sidecar: it stays as it is (a rewrite writes it in fewer) */
		sift_wt_abort(wt);
		sift_cseg_close(seg);
		return 0;
	}
	fresh.header_blk = sift_wt_write_seghdr(wt, old, list, nlist);
	release[nrelease++] = (SiftExtent) {entry->header_blk, 1};
	fresh.flags |= SIFT_SEG_NET;
	fresh.bytes = entry->bytes + (blocks - (seg->sr.npages - keep_pages)) * BLCKSZ;
	sift_wt_finish(wt);
	sift_wt_publish(wt, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw_dir,.release = release,.nrelease = nrelease,.extra = InvalidBuffer});
	sift_cseg_close(seg);
	return blocks + 1;
}

/* The column segments that have dead rows the net pre-aggregates they hold do not know get new ones (VACUUM, with the maintenance lock held) */
void
sift_tbl_net_refresh(Relation rel, SiftTblRel *st)
{
	SiftDirSnapshot *snap;

	if (RecoveryInProgress() || rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
#ifdef SIFT_TESTING
	if (!sift_test_col_net)
		return;
#endif
	sift_tbl_refresh(rel, st);
	snap = palloc(sizeof(SiftDirSnapshot));
	*snap = st->dir;
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		bool		due;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || !(snap->segs[i].flags & SIFT_SEG_DIRTY))
			continue;
		seg = sift_cseg_open(rel, &snap->segs[i], CurrentMemoryContext);
		due = sift_cseg_net_due(seg);
		sift_cseg_close(seg);
		if (due)
		{
			TimestampTz start = GetCurrentTimestamp();
			uint64		written = net_write(rel, &snap->segs[i]);

			if (written > 0)
				sift_log_maint(rel, "wrote the net pre-aggregates of", start, snap->segs[i].ndocs, written * BLCKSZ);
			sift_tbl_refresh(rel, st);
			*snap = st->dir;
		}
	}
	pfree(snap);
}
