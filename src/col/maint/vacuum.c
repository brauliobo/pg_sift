/*
 * col/maint/vacuum.c - VACUUM of a sift_col index: bulk delete of segments and write buffer
 * dead rows, the overflow, the end of the file the vacuum may cut
 * the WAL budget of a vacuum
 */
#include "postgres.h"
#include "access/parallel.h"
#include "col/maint/maint_int.h"
#include "col/vstore/vstore.h"

double		sift_col_auto_merge_wal_pct = 5.0;

bool
maint_is_col(Relation index)
{
	return sift_col_is_index(index);
}

void
maint_bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);

	sift_cseg_bulkdelete(seg, cb, cb_state, stats);
	sift_cseg_close(seg);
}

void
maint_bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	sift_cwbuf_bulkdelete(index, e, sift_col_rel(index)->schema, cb, cb_state, stats);
}

typedef struct HeapDead
{
	IndexFetchTableData *scan;
	TupleTableSlot *slot;
	BlockNumber nblocks;
} HeapDead;

/* The heap row of a TID is dead to every snapshot (or its page is gone): what an index entry may be dropped for */
static bool
heap_row_dead(const ItemPointerData *tid, void *arg)
{
	HeapDead   *h = arg;
	ItemPointerData t = *tid;
	bool		again = false,
				all_dead = false;

	if (ItemPointerGetBlockNumber(tid) >= h->nblocks)
		return true;
	table_index_fetch_tuple(h->scan, &t, SnapshotAny, h->slot, &again, &all_dead);
	return all_dead;
}

/*
 * VACUUM that skipped the vacuuming of the indexes (dead rows on under 2% of the heap pages) never called ambulkdelete, so the rows it
 * dropped are live in the index, and a unit that flags an overflow value keeps the flag after the rows that held it are repaired. The
 * rows that hold such a value are the only ones looked up in the heap here; the merge that follows rewrites the segments the flag only
 * dead rows account for (sift_cseg_stale_ovf).
 */
void
maint_reap_overflow(Relation index, Relation heap)
{
	const SiftColLeafStats *ls = sift_col_leaf_stats(index);
	SiftDirSnapshot *snap;
	HeapDead	h;
	bool		flagged = false;

	for (int c = 0; c < sift_col_index_defs(index)->ncols; c++)
		flagged |= ls[c].has_ovf;
	if (!flagged)
		return;
	snap = palloc(sizeof(SiftDirSnapshot));
	h = (HeapDead) {.scan = sift_table_index_fetch_begin(heap),.slot = table_slot_create(heap, NULL),.nblocks = RelationGetNumberOfBlocks(heap)};
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);

			sift_cseg_reap_overflow(seg, heap_row_dead, &h);
			sift_cseg_close(seg);
		}
	ExecDropSingleTupleTableSlot(h.slot);
	table_index_fetch_end(h.scan);
	pfree(snap);
}

bool
maint_vacuum_truncates(Relation index)
{
	return sift_col_options(index)->vacuum_truncate;
}

/*
 * The rolling rewrite of a key-cut index (roll.c) ends with the blocks of its last step free (the partition it took and the overlay rows that waited for it), and the
 * promotions and the first step of the next pass write into them. The VACUUM leaves as many free blocks inside the file as a partition holds, or the largest segment when it is smaller (and cuts the free tail):
 * it moves down only the blocks above the end that holds no more (the online shrink, bm25/space.c), where a copy of the segments that end the file down into the holes
 * wrote whole segments to cut a part of one, as the segments of the steps lie in each other's extents.
 */
static uint64
maint_rolling_keep(Relation index)
{
	SiftDirSnapshot *snap;
	uint64		largest = 0;

	if (maint_part_kb == 0 || !sift_col_is_index(index) || !sift_col_key_cut(sift_col_rel(index)))
		return 0;
	snap = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	maint_use_partitions(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			largest = Max(largest, snap->segs[i].bytes);
	pfree(snap);
	return Min(largest, (uint64) maint_part_kb * 1024) / BLCKSZ;
}

/*
 * VACUUM moves the segments that end the file down into free blocks, a copy of each (its WAL is the segment's bytes) that also makes the segment new: the
 * WAL budget of the automatic rewrite counts from the write of the oldest large segment, so every move put it off by another 100 / sift.col_auto_merge_wal_pct_
 * times the segment, and the churn it was to answer went on. An index that is due for a rewrite gets the segments written into those free blocks by it, when the
 * automatic rewrite will start: one that waits for its WAL budget or for the snapshots that hold the blocks of the last rewrite is no reason to leave the file loose,
 * and neither is a write buffer that is due alone (the budget lets its promotion through): promoting it moves no segment. A rewrite that is not coming has
 * nothing to fill the holes of a file the last one made twice the index (the last pass of a build's runs, a horizon that held the blocks of a rewrite and passed),
 * so the VACUUM returns them, whatever the copy costs: it is bounded by sift.vacuum_rewrite_limit a round and happens once.
 * The result is the free blocks the VACUUM leaves inside the file: what maint_rolling_keep says for a key-cut index in partitions when the rewrite is not coming (it moves
 * down only what is over it), PG_UINT64_MAX when the rewrite is (it moves nothing), 0 for the rest (the space step as it was).
 */
uint64
maint_space_keep(Relation index)
{
	const char *why;
	uint64		keep = maint_rolling_keep(index);

	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return keep;
	why = maint_rewrite_check(index, true, NULL);
	return why != NULL && maint_rewrite_all(why) ? PG_UINT64_MAX : keep;
}

/* The LSN of the write of a segment: its header block, written once with the rest of the segment */
XLogRecPtr
maint_header_lsn(Relation index, const SiftSegEntry *e)
{
	Buffer		buf = ReadBuffer(index, e->header_blk);
	XLogRecPtr	lsn;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	lsn = PageGetLSN(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	return lsn;
}

/*
 * A rewrite writes about the bytes of the segments. The automatic one waits until the server has written 100 / sift.col_auto_merge_wal_pct times that
 * since its oldest large segment was written (the last rewrite, or the build): the WAL of the churn, whatever the width of the rows, the full-page
 * images or the other indexes make it, is the measure of the churn that a rewrite is worth. The segments of a wal_level = minimal cluster carry no LSN.
 */
bool
maint_wal_budget_spent(const XLogRecPtr *lsn, const uint64 *sbytes, int n, uint64 bytes)
{
	uint64		large = 0;
	XLogRecPtr	base = InvalidXLogRecPtr,
				now;

	if (sift_col_auto_merge_wal_pct <= 0 || RecoveryInProgress())
		return false;
	for (int i = 0; i < n; i++)
		large = Max(large, sbytes[i]);
	for (int i = 0; i < n; i++)
		if (sbytes[i] * 2 >= large && lsn[i] != InvalidXLogRecPtr && (base == InvalidXLogRecPtr || lsn[i] < base))
			base = lsn[i];
	now = GetXLogInsertRecPtr();
	return base != InvalidXLogRecPtr && now > base && (double) (now - base) * sift_col_auto_merge_wal_pct < 100.0 * (double) bytes;
}

IndexBulkDeleteResult *
sift_col_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	return sift_mops_bulkdelete(&maint_col_ops, info, stats, callback, callback_state);
}

IndexBulkDeleteResult *
sift_col_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	IndexBulkDeleteResult *result = sift_mops_vacuumcleanup(&maint_col_ops, info, stats);

	if (!IsParallelWorker() && !RecoveryInProgress())
		sift_vstore_vacuum(info->index);
	return result;
}
