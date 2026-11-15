/*
 * bm25/maint_vacuum.c - the VACUUM callbacks of a maintained index (bulkdelete and vacuumcleanup), with the merge and the truncation they end with; an index that finds its
 * own damage leaves service and the VACUUM goes on (core/quarantine.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "bm25/maint.h"
#include "bm25/maint_int.h"
#include "core/errctx.h"
#include "core/live.h"
#include "core/ops.h"
#include "core/progress.h"
#include "core/quarantine.h"
#include "core/sql.h"
#include "core/visibility.h"

typedef struct BulkDelete
{
	const SiftMaintOps *ops;
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
} BulkDelete;

static void
bulkdelete_index(void *arg)
{
	BulkDelete *bd = arg;
	const SiftMaintOps *ops = bd->ops;
	Relation	index = bd->info->index;
	IndexBulkDeleteResult *stats = bd->stats;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftErrCtx	ctx;
	int			order[SIFT_MAX_SEGMENTS];
	double		removed = stats->tuples_removed;

	sift_maint_lock(index, true);
	sift_errctx_push(&ctx, RelationGetRelid(index));
	sift_dir_read(index, snap);
	for (int k = 0, n = sift_dir_clear_order(snap, order); k < n; k++)
	{
		if (snap->segs[order[k]].kind == SIFT_SEG_IMMUTABLE)
			ops->bulkdelete_immutable(index, &snap->segs[order[k]], bd->callback, bd->callback_state, stats);
		else
			ops->bulkdelete_wbuf(index, &snap->segs[order[k]], bd->callback, bd->callback_state, stats);
		SIFT_CLEAR_TEST_POINT();
	}
	/*
	 * The tombstones of the entries removed go last, whatever the order of the directory: a reader that took the log and then the liveness of an entry that is now
	 * cleared sees the tombstone or the clear bit, never neither.
	 */
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_TOMB && ops->bulkdelete_tomb)
			ops->bulkdelete_tomb(index, &snap->segs[i], bd->callback, bd->callback_state, stats);
	/* The heap marks pages all-visible only after this returns: a scan that reads the map after that finds the version moved (sift_vis_begin) */
	if (stats->tuples_removed > removed)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	sift_errctx_pop(&ctx);
	sift_maint_unlock(index);
	pfree(snap);
}

IndexBulkDeleteResult *
sift_mops_bulkdelete(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
					 void *callback_state)
{
	BulkDelete	bd = {ops, info, stats ? stats : palloc0(sizeof(IndexBulkDeleteResult)), callback, callback_state};

	sift_quarantine_guard(info->index, SIFT_SKIP_VACUUM, bulkdelete_index, &bd);
	return bd.stats;
}

/* The merges of VACUUM: until the policy asks for none, as a build that left many segments needs, or just one; stops at a failed step */
static void
vacuum_merge(const SiftMaintOps *ops, Relation index)
{
	uint64		busy = 0;
	uint64		start = maint_written_bytes();
	uint64		limit = (uint64) sift_vacuum_rewrite_limit << 20;

	while (ops->merge_next(index, false, SIFT_GOAL_VACUUM, false, &busy) == SIFT_STEP_DONE && ops->vacuum_to_fixed_point)
	{
		CHECK_FOR_INTERRUPTS();
		if (ops->vacuum_budget && maint_written_bytes() - start >= limit)
			break;
	}
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftErrCtx	ctx;
	bool		began;

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
	vacuum_merge(ops, index);
	SIFT_TEST_GATE(sift_test_cleanup_gate, 2);
	if (ops->vacuum_refresh)
		ops->vacuum_refresh(index);
	sift_recycle(index);
	sift_maint_orphan_sweep(index);
	sift_progress_phase(index, "space");
	if (ops->vacuum_truncate && ops->vacuum_truncate(index))
	{
		uint64		keep = ops->vacuum_space_keep ? ops->vacuum_space_keep(index) : 0;
		bool		leave = keep > 0;
		bool		defer = keep == PG_UINT64_MAX;

		if (leave && !defer)
		{
			/* the segments move only as far as the blocks above the end that keeps `keep` free ones inside the file: the cut after it takes that end, and the VACUUM copied what it cuts, not the segments */
			BlockNumber end = sift_maint_shrink_end(index, keep);

			if (end > 0)
			{
				sift_maint_shrink_leaving(end);
				(void) online_shrink(ops, index, info->message_level);
				sift_maint_shrink_leaving(0);
				(void) sift_maint_cut_released(index, info->message_level);
			}
		}
		else if (!leave)
			(void) online_shrink(ops, index, info->message_level);
		sift_maint_space_rounds(index, NULL, info->message_level, leave ? 0 : (uint64) sift_vacuum_rewrite_limit << 20);
		if (!leave)
			online_fallback(ops, index, info->message_level);
	}
	sift_progress_end(began);
	sift_errctx_pop(&ctx);
	sift_maint_unlock(index);
	if (ops->vacuum_done)
		ops->vacuum_done(index);
	sift_dir_read(index, snap);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = false;
	if (stats->num_index_tuples == 0)
		for (int i = 0; i < snap->nsegs; i++)
			stats->num_index_tuples += snap->segs[i].ndocs;
	pfree(snap);
}

IndexBulkDeleteResult *
sift_mops_vacuumcleanup(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	VacuumCleanup vc;

	if (info->analyze_only)
		return stats;
	vc = (VacuumCleanup) {ops, info, stats ? stats : palloc0(sizeof(IndexBulkDeleteResult)), stats == NULL};
	sift_quarantine_guard(info->index, SIFT_SKIP_VACUUM, vacuumcleanup_index, &vc);
	return vc.stats;
}
