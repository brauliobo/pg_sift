/*
 * core/maint/vacuum.c - the VACUUM callbacks of a maintained index (bulkdelete and vacuumcleanup), with the merge and the truncation they end with; an index that finds its
 * own damage leaves service and the VACUUM goes on (core/quarantine/quarantine.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "core/maint/maint.h"
#include "core/maint/int.h"
#include "core/maint/sched.h"
#include "core/errctx.h"
#include "core/live/live.h"
#include "core/ops.h"
#include "core/progress.h"
#include "core/quarantine/quarantine.h"
#include "core/sql/sql.h"
#include "core/visibility/visibility.h"
#include "core/memtrack.h"

typedef struct BulkDelete
{
	const SiftMaintOps *ops;
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	ResourceOwner caller;
} BulkDelete;

/* The callback is the caller's (CREATE INDEX CONCURRENTLY feeds a sort that spills): what it opens belongs to the caller's resource owner, not to the subtransaction of the guard */
static bool
caller_callback(ItemPointer tid, void *state)
{
	BulkDelete *bd = state;
	ResourceOwner guarded = CurrentResourceOwner;
	bool		dead;

	CurrentResourceOwner = bd->caller;
	dead = bd->callback(tid, bd->callback_state);
	CurrentResourceOwner = guarded;
	return dead;
}

static void
bulkdelete_index(void *arg)
{
	BulkDelete *bd = arg;
	const SiftMaintOps *ops = bd->ops;
	Relation	index = bd->info->index;
	IndexBulkDeleteResult *stats = bd->stats;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift index vacuum");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *snap;
	SiftErrCtx	ctx;
	int		   *order;
	SiftMemTracker mt_index_vacuum;

	sift_mem_init_sub(&mt_index_vacuum, "index_vacuum", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	double		removed = stats->tuples_removed;
	bool		out = false;

	sift_maint_lock(index, true);
	sift_errctx_push(&ctx, RelationGetRelid(index));
	snap = sift_dir_read(index);
	order = palloc(Max(snap->nsegs, 1) * sizeof(int));
	for (int k = 0, n = sift_dir_clear_order(snap, order); k < n; k++)
	{
		SIFT_BULKDELETE_GATE();
		if ((out = sift_index_known_out(index)))
			break;
		if (snap->segs[order[k]].kind == SIFT_SEG_IMMUTABLE)
			ops->bulkdelete_immutable(index, &snap->segs[order[k]], caller_callback, bd, stats);
		else
			ops->bulkdelete_wbuf(index, &snap->segs[order[k]], caller_callback, bd, stats);
		sift_mem_assert_within(&mt_index_vacuum);
		SIFT_CLEAR_TEST_POINT();
	}
	/*
	 * The tombstones of the entries removed go last, whatever the order of the directory: a reader that took the log and then the liveness of an entry that is now
	 * cleared sees the tombstone or the clear bit, never neither. An index that left service while the segments were walked is not read again before its rebuild: the walk ends there.
	 */
	for (int i = 0; i < snap->nsegs && !out; i++)
		if (snap->segs[i].kind == SIFT_SEG_TOMB && ops->bulkdelete_tomb)
			ops->bulkdelete_tomb(index, &snap->segs[i], caller_callback, bd, stats);
	/* The heap marks pages all-visible only after this returns: a scan that reads the map after that finds the version moved (sift_vis_begin) */
	if (stats->tuples_removed > removed && !out)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	sift_errctx_pop(&ctx);
	sift_maint_unlock(index);
	pfree(snap);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	if (out)
		sift_quarantine_stopped(index, SIFT_SKIP_VACUUM);
}

IndexBulkDeleteResult *
sift_mops_bulkdelete(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
					 void *callback_state)
{
	BulkDelete	bd = {ops, info, stats ? stats : palloc0(sizeof(IndexBulkDeleteResult)), callback, callback_state, CurrentResourceOwner};

	sift_quarantine_run(info->index, SIFT_SKIP_VACUUM, bulkdelete_index, &bd);
	return bd.stats;
}

/* The merges of VACUUM: until the policy asks for none, as a build that left many segments needs, or just one; stops at a failed step. True when the rewrite limit stopped them */
static bool
vacuum_merge(const SiftMaintOps *ops, Relation index)
{
	uint64		busy = 0;
	uint64		start = maint_written_bytes();
	uint64		limit = (uint64) sift_vacuum_rewrite_limit << 20;

	while (ops->merge_next(index, false, SIFT_GOAL_VACUUM, false, &busy) == SIFT_STEP_DONE && ops->vacuum_to_fixed_point)
	{
		CHECK_FOR_INTERRUPTS();
		if (ops->vacuum_budget && maint_written_bytes() - start >= limit)
			return true;
	}
	return false;
}

/* The merges that the rewrite limit left are the background maintenance's, which paces them; a temporary index is its session's alone */
static void
request_merges(Relation index)
{
	if (index->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		(void) sift_maint_enqueue_here(&(SiftMaintReq) {.db = MyDatabaseId,.rel = RelationGetRelid(index),.kind = SIFT_MK_BM25_MERGE,.tier = SIFT_MAINT_TIER_DEFAULT});
}

typedef struct VacuumCleanup
{
	const SiftMaintOps *ops;
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	bool		skipped;
} VacuumCleanup;

static void
vacuumcleanup_index(void *arg)
{
	VacuumCleanup *vc = arg;
	const SiftMaintOps *ops = vc->ops;
	IndexVacuumInfo *info = vc->info;
	IndexBulkDeleteResult *stats = vc->stats;
	bool		skipped = vc->skipped;
	Relation	index = info->index;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift index vacuum");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *snap;
	SiftErrCtx	ctx;
	bool		began,
				limited;
	SiftMemTracker mt_index_vacuum;

	sift_mem_init_sub(&mt_index_vacuum, "index_vacuum", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));

	sift_maint_lock(index, true);
	sift_errctx_push(&ctx, RelationGetRelid(index));
	began = sift_progress_begin(index, SIFT_JOB_VACUUM);
	sift_progress_phase(index, "promote");
	if (ops->vacuum_seal)
		ops->vacuum_seal(index);
	promote_locked(ops, index, false, false);
	if (ops->vacuum_log)
		ops->vacuum_log(index);
	if (skipped && ops->vacuum_skipped)
		ops->vacuum_skipped(index, info->heaprel);
	if (ops->vacuum_heap)
		ops->vacuum_heap(index, info->heaprel);
	if (ops->vacuum_net)
		ops->vacuum_net(index);
	sift_progress_phase(index, "merge");
	limited = vacuum_merge(ops, index);
	sift_mem_assert_within(&mt_index_vacuum);
	SIFT_TEST_GATE(sift_test_cleanup_gate, 2);
	if (ops->vacuum_refresh)
		ops->vacuum_refresh(index);
	sift_recycle(index);
	sift_maint_orphan_sweep(index);
	sift_progress_phase(index, "space");
	if (ops->vacuum_truncate && ops->vacuum_truncate(index))
	{
		(void) online_shrink(ops, index, info->message_level);
		sift_maint_space_rounds(index, NULL, info->message_level, (uint64) sift_vacuum_rewrite_limit << 20);
		online_fallback(ops, index, info->message_level);
	}
	sift_progress_end(began);
	sift_errctx_pop(&ctx);
	sift_maint_unlock(index);
	if (limited && ops->worker == SIFT_WK_BM25_MAINT)
		request_merges(index);
	if (ops->vacuum_done)
		ops->vacuum_done(index);
	snap = sift_dir_read(index);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = false;
	if (stats->num_index_tuples == 0)
		for (int i = 0; i < snap->nsegs; i++)
			stats->num_index_tuples += snap->segs[i].ndocs;
	pfree(snap);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

IndexBulkDeleteResult *
sift_mops_vacuumcleanup(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	VacuumCleanup vc;

	if (info->analyze_only)
		return stats;
	vc = (VacuumCleanup) {ops, info, stats ? stats : palloc0(sizeof(IndexBulkDeleteResult)), stats == NULL};
	sift_quarantine_run(info->index, SIFT_SKIP_VACUUM, vacuumcleanup_index, &vc);
	return vc.stats;
}
