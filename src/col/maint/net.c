/*
 * col/maint/net.c - the net pre-aggregates of the dirty units of a segment (col/seg/net.c), written by VACUUM instead of a rewrite of the segment.
 *
 * The pages of the sidecar go after the segment's own stream, the segment keeps its generation and its header page is written again at a new block with the extents
 * it has now; the directory entry and the pages the old header and the old sidecar took are switched and released together (sift_wt_publish), as the relocation of a
 * segment does. A crash before the switch leaves the new pages unreferenced (the orphan sweep returns them); after it the segment has the new sidecar whole.
 */
#include "postgres.h"
#include "utils/timestamp.h"
#include "col/maint/maint_int.h"
#include "bm25/maint_int.h"
#include "core/ops.h"

typedef struct NetStep
{
	const SiftSegEntry *entry;
	uint64		written;		/* pages */
} NetStep;

/*
 * The sidecar of seg as it is now, written after the segment's own stream (the old sidecar's pages are left out and released with the old header page), and the header page that
 * lists the extents, switched in for the old one by sift_wt_publish. Returns the pages written (the sidecar's and the header's); 0 with *nextents set to the extents the header
 * would need, when it cannot list that many: the stream is written, nothing is published, and the caller aborts the write or raises the error. The caller closes seg.
 */
uint64
sift_col_net_write(SiftWriteTarget *wt, SiftColSegment *seg, const SiftSegEntry *entry, int *nextents)
{
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
	/* the extents of the segment's own stream, then the sidecar's; the old sidecar's are released */
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
		*nextents = nlist;
		return 0;
	}
	fresh.header_blk = sift_wt_write_seghdr(wt, old, list, nlist);
	release[nrelease++] = (SiftExtent) {entry->header_blk, 1};
	fresh.flags |= SIFT_SEG_NET;
	fresh.bytes = entry->bytes + (blocks - (seg->sr.npages - keep_pages)) * BLCKSZ;
	sift_wt_finish(wt);
	sift_wt_publish(wt, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw_dir,.release = release,.nrelease = nrelease,.extra = InvalidBuffer});
	return blocks + 1;
}

static void
net_write(Relation index, SiftWriteTarget *wt, const void *arg)
{
	NetStep    *st = (NetStep *) arg;
	SiftColSegment *seg = sift_cseg_open(index, st->entry, CurrentMemoryContext);
	int			nextents;

	st->written = sift_col_net_write(wt, seg, st->entry, &nextents);
	if (st->written == 0)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the net pre-aggregates need %d extents, the header holds %d", nextents, (int) SIFT_MAX_EXTENTS),
						errhint("Merge the segments first (sift.col_merge) so that the segment is written in fewer extents.")));
	sift_cseg_close(seg);
}

/*
 * The segments that have dead rows the net pre-aggregates they hold do not know get new ones (VACUUM, with the maintenance lock held, before its rewrite is
 * decided: the rule counts the units that the sidecar answers for as clean).
 */
void
maint_net_refresh(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint64		largest = 0;

	if (RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
#ifdef SIFT_TESTING
	if (!sift_test_col_net)
		return;
#endif
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			largest = Max(largest, snap->segs[i].ndocs);
	for (int i = 0; i < snap->nsegs; i++)
	{
		NetStep		st = {.entry = &snap->segs[i]};
		SiftColSegment *seg;
		bool		due;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || !(snap->segs[i].flags & SIFT_SEG_DIRTY))
			continue;
		/* an overlay is merged into a larger segment before its dead rows are worth a sidecar of its own: it would be written again by each VACUUM that kills one more row */
		if (maint_is_overlay(snap->segs[i].ndocs, largest))
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		due = sift_cseg_net_due(seg);
		sift_cseg_close(seg);
		if (due)
		{
			TimestampTz start = GetCurrentTimestamp();

			if (run_step(index, false, net_write, &st) == SIFT_STEP_DONE && st.written > 0)
				sift_log_maint(index, "wrote the net pre-aggregates of", start, snap->segs[i].ndocs, st.written * BLCKSZ);
		}
	}
	pfree(snap);
}
