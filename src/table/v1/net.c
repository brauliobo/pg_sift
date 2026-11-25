/*
 * table/v1/net.c - the net pre-aggregates of the dirty units of the column segments of a sift_table (col/v1/seg/net.c, docs/COL-PREAGG.md section 13), written by VACUUM
 * as col/v1/maint/net.c writes them for an index: after the liveness bits of the dead rows are cleared and the merges that rewrite a segment are done, the segments
 * that kept a dead row get the cube, the rollups and the sums of their live rows after their own stream, and the directory entry says SIFT_SEG_NET.
 * The pages go in the table's own file; a crash before the switch leaves them unreferenced (the orphan sweep returns them).
 */
#include "postgres.h"

#include "access/xlog.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/am.h"
#include "col/v1/seg.h"
#include "core/memtrack.h"
#include "core/ops.h"
#include "vacuum_int.h"

/* The segment's own stream, then the sidecar's pages; the old sidecar's are released with the old header page */
static uint64
net_write(Relation rel, const SiftSegEntry *entry, SiftColSegment *seg, StringInfo net)
{
	SiftWriteTarget *wt = sift_wt_begin(rel, SIFT_WRITE_SHARED);
	int			nextents;
	uint64		pages = sift_col_net_write(wt, seg, entry, net, &nextents);

	/* the segment is written in too many extents for the header to list the sidecar: it stays as it is (a rewrite writes it in fewer) */
	if (pages == 0)
		sift_wt_abort(wt);
	return pages;
}

/* The net pre-aggregates of one segment when it is due, built and written in segcx, which the next segment starts empty; whether they were written */
static bool
net_segment(Relation rel, const SiftSegEntry *entry, MemoryContext segcx)
{
	MemoryContext old = MemoryContextSwitchTo(segcx);
	SiftColSegment *seg = sift_cseg_open(rel, entry, segcx);
	StringInfo	net = sift_cseg_net_due(seg) ? sift_cseg_net_build(seg) : NULL;
	bool		wrote = net != NULL;

	if (wrote)
	{
		TimestampTz start = GetCurrentTimestamp();
		uint64		written = net_write(rel, entry, seg, net);

		if (written > 0)
			sift_log_maint(rel, "wrote the net pre-aggregates of", start, entry->ndocs, written * BLCKSZ);
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(old);
	MemoryContextReset(segcx);
	return wrote;
}

/* The column segments that have dead rows the net pre-aggregates they hold do not know get new ones (VACUUM, with the maintenance lock held) */
void
sift_tbl_net_refresh(Relation rel, SiftTblRel *st)
{
	SiftDirSnapshot *snap;
	MemoryContext segcx;
	bool		wrote = false;

	if (RecoveryInProgress() || rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
#ifdef SIFT_TESTING
	if (!sift_test_col_net)
		return;
#endif
	sift_tbl_refresh(rel, st);
	snap = sift_dir_copy(st->dir);
	segcx = sift_mem_allocset_create(CurrentMemoryContext, "sift_table net pass");
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_DIRTY))
			wrote |= net_segment(rel, &snap->segs[i], segcx);
	MemoryContextDelete(segcx);
	if (wrote)
		sift_tbl_refresh(rel, st);
	pfree(snap);
}
