/*
 * col/maint/vacuum.c - VACUUM of a sift_col index: bulk delete of segments and write buffer
 * dead rows, the overflow, the end of the file the vacuum may cut
 */
#include "postgres.h"
#include "access/parallel.h"
#include "col/maint/maint_int.h"
#include "col/scope.h"
#include "col/side.h"
#include "core/live.h"
#include "core/quarantine.h"
#include "col/vstore/vstore.h"

bool
maint_is_col(Relation index)
{
	return sift_col_is_index(index);
}

void
maint_bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftColSegment *seg;

	SIFT_TEST_MUTANT(3, return);
	seg = sift_cseg_open(index, e, CurrentMemoryContext);
	sift_cseg_bulkdelete(seg, cb, cb_state, stats);
	sift_cseg_close(seg);
}

void
maint_bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SIFT_TEST_MUTANT(3, return);
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
	int		   *order;
	bool		flagged = false;

	for (int c = 0; c < sift_col_index_defs(index)->ncols; c++)
		flagged |= ls[c].has_ovf;
	if (!flagged)
		return;
	h = (HeapDead) {.scan = sift_table_index_fetch_begin(heap),.slot = table_slot_create(heap, NULL),.nblocks = RelationGetNumberOfBlocks(heap)};
	snap = sift_dir_read(index);
	order = palloc(Max(snap->nsegs, 1) * sizeof(int));
	for (int k = 0, n = sift_dir_clear_order(snap, order); k < n; k++)
		if (snap->segs[order[k]].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[order[k]], CurrentMemoryContext);

			sift_cseg_reap_overflow(seg, heap_row_dead, &h);
			sift_cseg_close(seg);
		}
	ExecDropSingleTupleTableSlot(h.slot);
	table_index_fetch_end(h.scan);
	pfree(order);
	pfree(snap);
}

bool
maint_vacuum_truncates(Relation index)
{
	return sift_col_options(index)->vacuum_truncate;
}

IndexBulkDeleteResult *
sift_col_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	/* the validation pass of a concurrent build is the only caller that asks for progress reports: VACUUM does not */
	bool		validating = info->report_progress && sift_col_rel(info->index)->nscope > 0;

	if (validating)
		sift_scope_journals_validate();
	stats = sift_mops_bulkdelete(&maint_col_ops, info, stats, callback, callback_state);
	if (validating)
		sift_col_side_build(info->index);
	return stats;
}

static void
vstore_vacuum_index(void *index)
{
	sift_vstore_vacuum(index);
}

IndexBulkDeleteResult *
sift_col_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	IndexBulkDeleteResult *result = sift_mops_vacuumcleanup(&maint_col_ops, info, stats);

	if (!IsParallelWorker() && !RecoveryInProgress() && !sift_index_known_out(info->index))
		sift_quarantine_guard(info->index, SIFT_SKIP_VACUUM, vstore_vacuum_index, info->index);
	return result;
}
