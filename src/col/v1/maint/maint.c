/*
 * col/v1/maint/maint.c - the maintenance of a sift_col index: the entry points of a seal, a make-room,
 * a promotion, a merge step and the background workers
 * the files next to it hold what they run
 */
#include "postgres.h"
#include "access/table.h"
#include "catalog/index.h"
#include "port/pg_bitutils.h"
#include "utils/snapmgr.h"
#include "core/limits.h"
#include "core/maint/vacuum.h"
#include "core/memtrack.h"
#include "core/ops.h"
#include "core/worker.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/timestamp.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/wbuf.h"
#include "col/v1/maint/int.h"

/* The automatic promotion and merge (sift.col_auto_merge*, registered in col/am/am.c) */

double		sift_col_auto_merge_fragmentation = 2.0;

/*
 * The size of a partition in effect for the directory in hand: the smaller of sift.vacuum_rewrite_limit (for an index over it, so that the rewrite of a large index is a
 * step for each partition, the lock let go between them, and not one step for all of it) and the share of the memory a writer of a merge holds within maintenance_work_mem (two
 * fifths of it, MAINT_MEM_RATIO bytes of memory for each byte of segment: a global order weighs the key and address of each of its rows until the last unit is in). 0: no partitions.
 */
int	maint_part_kb = 0;
#ifdef SIFT_TESTING
int			sift_test_col_partition_kb = 0;
int			sift_test_col_pass_kb = 0;
#endif

/* the bytes of overlapping runs a pass of their rewrite takes: sixteen times the memory of a merge, so that the passes are few and each leaves the index valid */
uint64
maint_pass_limit(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_pass_kb > 0)
		return (uint64) sift_test_col_pass_kb * 1024;
#endif
	return (uint64) sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) * 16;
}

int
maint_part_kb_for(uint64 bytes)
{
	int			kb = bytes > sift_vacuum_rewrite_cap() ? Min(sift_vacuum_rewrite_limit, INT_MAX / 1024) * 1024 : 0;
	uint64		mem_kb = (uint64) (sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) * 2 / 5 / 1024 / MAINT_MEM_RATIO);

#ifdef SIFT_TESTING
	if (sift_test_col_partition_kb > 0)
		return sift_test_col_partition_kb;
#endif
	if (bytes / 1024 > mem_kb && (kb == 0 || mem_kb < (uint64) kb))
		kb = (int) Min(mem_kb, (uint64) INT_MAX);
	return kb;
}

void
maint_use_partitions(Relation index, const SiftDirSnapshot *snap)
{
	uint64		bytes = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			bytes += snap->segs[i].bytes;
	maint_part_kb = maint_part_kb_for(bytes);
}

/*
 * A partition is an immutable segment of at least half of the partition size; the smaller ones are the overlays that promotions and
 * rewrites of a partition left over the same superblocks. 0: no partitions, the rewrite is of every segment into one.
 */
bool
maint_is_partition(const SiftSegEntry *e)
{
	return maint_part_kb > 0 && e->bytes >= (uint64) maint_part_kb * 512;
}

uint64
maint_overlay_rows(const SiftSegEntry *seg, int n, const SiftSegEntry *skip, int nskip)
{
	uint64		largest = 0,
				rows = 0;

	for (int i = 0; i < n; i++)
		if (seg[i].kind == SIFT_SEG_IMMUTABLE)
			largest = Max(largest, seg[i].ndocs);
	for (int i = 0; i < n; i++)
	{
		bool		replaced = false;

		for (int k = 0; k < nskip; k++)
			replaced |= skip[k].gen == seg[i].gen;
		if (seg[i].kind == SIFT_SEG_IMMUTABLE && !replaced && maint_is_overlay(seg[i].ndocs, largest))
			rows += seg[i].ndocs;
	}
	return rows;
}

void
maint_set_unordered(SiftCPaggSpec *spec, const SiftColSchema *schema, uint64 overlay_rows, uint64 index_rows)
{
	uint64		cap = PG_UINT64_MAX;

	for (int c = 0; c < schema->ncols; c++)
		if (spec->gorder[c])
			cap = Min(cap, SIFT_CGORD_FREE_CAP(schema->cols[c].store != SIFT_COLS_INT));
	spec->unordered = spec->lean && spec->gorderauto && overlay_rows * 10 <= index_rows && overlay_rows <= cap;
}

/* The participants of the parallel build that is merging its own segments (col/v1/build_par.c); 0: one process merges, as VACUUM and the maintenance worker do */
int	maint_merge_workers = 0;
bool		maint_after_build = false;	/* the rewrite is the one right after the build: the choices of the build stand */
bool		maint_redecided = false;	/* the rewrite in hand has chosen again from the data (col/v1/maint/merge.c): once for all its steps */

/*
 * A relocation copies the pages of a segment as they are, so every unit of the new generation is the unit of the old one with the same number: the deletion records of the segment are
 * written again under the new generation (sift_col_dv_rekey). A testing build can name units that the move treats as cut again, whose records are carried to marks instead.
 */
static void
maint_relocated(Relation index, uint64 gen, uint64 gen2, uint64 recut)
{
	SiftDvMove	moves[65];
	uint32		start = 0;
	int			n = 0;

	for (; recut != 0; recut &= recut - 1)
	{
		uint32		unit = pg_rightmost_one_pos64(recut);

		if (unit > start)
			moves[n++] = (SiftDvMove) {gen, start, unit - start, gen2, start};
		start = unit + 1;
	}
	moves[n++] = (SiftDvMove) {gen, start, PG_UINT32_MAX - start, gen2, start};
	sift_col_dv_rekey(index, &gen, 1, moves, n);
}

const SiftMaintOps maint_col_ops = {
	.name = "sift_col",
	.worker = SIFT_WK_COL_MAINT,
	.step_lock = ExclusiveLock,
	.vacuum_to_fixed_point = true,
	.vacuum_budget = true,
	.force_seals = true,
	.vacuum_truncate = maint_vacuum_truncates,
	.vacuum_seal = sift_cwbuf_seal_vacuum,
	.vacuum_rotate = sift_cwbuf_rotate,
	.is_index = maint_is_col,
	.seal = sift_cwbuf_seal,
	.promote_next = maint_promote_next,
	.merge_next = maint_merge_next,
	.bulkdelete_immutable = maint_bulkdelete_immutable,
	.bulkdelete_wbuf = maint_bulkdelete_wbuf,
	.bulkdelete_tomb = maint_bulkdelete_tomb,
	.vacuum_net = maint_net_refresh,
	.vacuum_heap = maint_roots_verify,
	.vacuum_log = maint_vacuum_log,
	.vacuum_skipped = maint_reap_overflow,
	.switching = sift_col_dv_switching,
	.switched = sift_col_dv_switched,
	.relocated = maint_relocated,
	.rewrite_due = maint_rewrite_due,
	.rewrite_all = maint_rewrite_all,
	.vacuum_done = maint_buffer_request,
};

Relation
sift_col_open_index(Oid indexoid, LOCKMODE lock)
{
	return sift_mops_open(&maint_col_ops, indexoid, lock);
}

/*
 * After a parallel build. The segments of its participants overlap (each holds some rows of every key and of every superblock), so the first VACUUM
 * rewrote them, and the rewrite left the index twice its size in the file: its output goes after the segments it replaced, and the free blocks in front of it
 * stay there for good while a standby streams (VACUUM does not cut the file then). The build reserved a gap in front of the segments (gap, gapn blocks,
 * released here): the merge writes into it, the segments it replaces are the free end, and the cut takes them. The file of a build is the one no query of
 * a standby can hold before the commit, so the cut waits for none. A gap too small leaves the rest of the output above the free blocks: the space step
 * brings it down.
 */
void
sift_col_build_merge(Relation index, BlockNumber gap, uint32 gapn, int workers)
{
	bool		merged;

	if (gapn > 0)
	{
		SiftExtent	ext = {gap, gapn};

		sift_dir_update(index, &(SiftDirChange) {.release = &ext,.nrelease = 1,.immediate = true,.extra = InvalidBuffer});
	}
#ifdef SIFT_TESTING
	if (sift_test_col_merge_workers >= 0)
		workers = Min(workers, sift_test_col_merge_workers);
#endif
	maint_merge_workers = workers;
	maint_after_build = true;
	PG_TRY();
	{
		merged = sift_mops_rewrite(&maint_col_ops, index) > 0;
	}
	PG_FINALLY();
	{
		maint_merge_workers = 0;
		maint_after_build = false;
	}
	PG_END_TRY();
	if (merged || gapn > 0)
	{
		sift_maint_lock(index, true);
		sift_recycle(index);
		sift_maint_orphan_sweep(index);
		if (maint_vacuum_truncates(index))
			sift_maint_space_rounds(index, NULL, DEBUG1, (uint64) RelationGetNumberOfBlocks(index) * BLCKSZ);
		sift_maint_unlock(index);
	}
}

void
sift_col_maint_after_seal(Relation index)
{
	sift_mops_after_seal(&maint_col_ops, index);
	if (sift_cwbuf_arena_asked_back(index))
		sift_mops_promote(&maint_col_ops, index, false);
}

bool
sift_col_maint_make_room(Relation index)
{
	return sift_mops_make_room(&maint_col_ops, index);
}

int
sift_col_maint_promote(Relation index, bool wait)
{
	return sift_mops_promote(&maint_col_ops, index, wait);
}

bool
sift_col_maint_merge_step(Relation index, bool wait)
{
	return sift_mops_merge_step(&maint_col_ops, index, wait);
}

/*
 * The re-baseline of a log that is due for it (sift_col_track_rebaseline), in a transaction of its own: it holds ShareUpdateExclusiveLock on the table, which keeps VACUUM
 * and the autovacuum worker waiting or skipping for as long as the transaction lasts, and the merges of the worker are longer. A table that is busy with a VACUUM is left to it (it re-baselines too).
 * Triggers that ENABLE TRIGGER left enabled for the origin only are enabled for all again first (sift_col_track_rearm), in a transaction of its own, which returns true for the caller to run the step again.
 */
static bool
rebaseline_step(Oid indexoid, MemoryContext cx, const char *volatile *name)
{
	Oid			heapoid;
	Relation	index;
	Relation	heap;
	bool		rearmed = false;
	bool		done = false;

	StartTransactionCommand();
	heapoid = IndexGetRelation(indexoid, true);
	if (OidIsValid(heapoid) && !RecoveryInProgress() && ConditionalLockRelationOid(heapoid, ShareUpdateExclusiveLock) &&
		(index = try_index_open(indexoid, RowExclusiveLock)) != NULL)
	{
		*name = MemoryContextStrdup(cx, RelationGetRelationName(index));
		if (sift_col_is_index(index))
		{
			TimestampTz start = GetCurrentTimestamp();

			heap = table_open(heapoid, AccessShareLock);
			/* triggers enabled for the origin only: the lock that ENABLE TRIGGER takes, if it is free this instant (held for a catalog row, never waited for), and the re-baseline in the next transaction */
			if (sift_col_track_rearmable(index, heap) && ConditionalLockRelationOid(heapoid, ShareRowExclusiveLock))
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				done = rearmed = sift_col_track_rearm(index, heap);
				PopActiveSnapshot();
				if (rearmed)
					sift_log_maint(index, "column track triggers enabled again", start, -1, -1);
			}
			else if ((done = sift_col_track_rebaseline(index, heap)))
				sift_log_maint(index, "column track re-baseline finished", start, -1, -1);
			if (done)
				sift_worker_ok(index, SIFT_JOB_TRACK, start);
			table_close(heap, NoLock);
		}
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
	return rearmed;
}

/*
 * The blocks a step that was killed or failed took and no segment lists (the sweep of VACUUM, which a table that is never vacuumed, or whose VACUUM is skipped for a lock, does not run)
 * go back to the free list in a transaction of their own, so that the next write reuses them; the free end they leave is cut in the next one, when a snapshot does not hold it.
 * The maintenance lock is taken only if free: a step in progress has blocks in no directory yet.
 */
static int64
sweep_step(Oid indexoid, bool cut)
{
	Relation	index;
	volatile int64 freed = 0;

	StartTransactionCommand();
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		if (sift_col_is_index(index) && !RecoveryInProgress() && cut)
			sift_mops_cut_free_end(&maint_col_ops, index);
		else if (sift_col_is_index(index) && !RecoveryInProgress() && sift_maint_lock(index, false))
		{
			freed = sift_maint_orphan_sweep(index);
			sift_maint_unlock(index);
			if (freed > 0)
				ereport(LOG, (errmsg("sift: returned %lld blocks that no segment lists to index \"%s\"", (long long) freed, RelationGetRelationName(index)),
							  errdetail("A rewrite or merge that was shut down or failed had written them.")));
		}
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
	return freed;
}

static void
reclaim_orphans(Oid indexoid)
{
	if (sweep_step(indexoid, false) > 0)
		sweep_step(indexoid, true);
}

/* The re-baseline that a read of a log asked for (sift_col_track_ask) */
void
sift_col_track_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;
	const char *volatile name = psprintf("%u", indexoid);

	PG_TRY();
	{
		if (rebaseline_step(indexoid, cx, &name))
			rebaseline_step(indexoid, cx, &name);
	}
	PG_CATCH();
	{
		sift_maint_worker_failed("sift_col", indexoid, name, cx);
	}
	PG_END_TRY();
}

/* The open write buffer sealed first, as the reads that decoded it or the VACUUM that found it large asked (maint_wbuf_read_due, maint_wbuf_share_due, judged again here: it may have been promoted since) */
static void
seal_step(Oid indexoid, MemoryContext cx, const char *volatile *name)
{
	Relation	index;

	StartTransactionCommand();
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		*name = MemoryContextStrdup(cx, RelationGetRelationName(index));
		if (sift_col_is_index(index) && (maint_wbuf_read_due(index) || maint_wbuf_share_due(index)))
			sift_cwbuf_seal(index, true);
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
}

/* This worker promoted or merged: a worker that found nothing to do in its whole stay reads and writes nothing more, and says nothing */
static bool col_worker_worked = false;

/*
 * One promotion or merge in a transaction of its own (sift_maint_worker_stay). The lock is busy when a VACUUM or a SQL merge is at it, which merge to the end,
 * and when an inserter promotes, which leaves the merges to the worker: the next look finds it free.
 */
static SiftStepResult
col_worker_step(Oid indexoid, TimestampTz start, bool last)
{
	MemoryContext cx = CurrentMemoryContext;
	const char *volatile name = psprintf("%u", indexoid);	/* the name of the index, once it is open: a failure before that names the oid */
	volatile SiftStepResult r = SIFT_STEP_IDLE;

	PG_TRY();
	{
		Relation	index;

		StartTransactionCommand();
		index = sift_try_open_index(indexoid, RowExclusiveLock);
		if (index != NULL && sift_col_is_index(index))
		{
			TimestampTz began = GetCurrentTimestamp();
			SiftJob		job = SIFT_JOB_PROMOTE;

			name = MemoryContextStrdup(cx, RelationGetRelationName(index));
			if (sift_col_maint_promote(index, false) > 0 || (job = SIFT_JOB_MERGE, sift_col_maint_merge_step(index, false)))
			{
				r = SIFT_STEP_DONE;
				col_worker_worked = true;
				sift_worker_ok(index, job, began);
			}
			else if (!sift_maint_lock(index, false))
				r = SIFT_STEP_BUSY;
			else
			{
				sift_maint_unlock(index);
				if (last && col_worker_worked)
					sift_mops_cut_free_end(&maint_col_ops, index);
			}
		}
		if (index != NULL)
			index_close(index, RowExclusiveLock);
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		sift_maint_worker_failed("sift_col", indexoid, name, cx);
		r = SIFT_STEP_FAILED;
	}
	PG_END_TRY();
	if (r == SIFT_STEP_FAILED)
		reclaim_orphans(indexoid);
	return r;
}

/* The marks of the log folded into the liveness runs when the reads have paid for it (sift_col_fold_tick asks for the worker, and any worker looks): judged here, the log may have been rewritten since */
static void
fold_step(Oid indexoid, MemoryContext cx, const char *volatile *name)
{
	Relation	index;

	StartTransactionCommand();
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		*name = MemoryContextStrdup(cx, RelationGetRelationName(index));
		if (sift_col_is_index(index) && sift_col_fold_due(index))
		{
			TimestampTz start = GetCurrentTimestamp();
			uint64		rows = sift_col_fold_marks(index);

			if (rows > 0)
			{
				sift_log_maint(index, "folded the marks of the log into", start, rows, -1);
				sift_worker_ok(index, SIFT_JOB_TRACK, start);
			}
		}
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
}

/* seal: the open write buffer is sealed first */
static void
maint_worker(Datum main_arg, bool seal)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;
	const char *volatile name = psprintf("%u", indexoid);
	volatile bool failed = false;

	if (!sift_maint_stay_begin(indexoid))
		return;

	PG_TRY();
	{
		if (rebaseline_step(indexoid, cx, &name))
			rebaseline_step(indexoid, cx, &name);
		if (seal)
			seal_step(indexoid, cx, &name);
		fold_step(indexoid, cx, &name);
	}
	PG_CATCH();
	{
		sift_maint_worker_failed("sift_col", indexoid, name, cx);
		failed = true;
	}
	PG_END_TRY();
	if (!failed)
		sift_maint_worker_stay(indexoid, start, col_worker_step);
}

void
sift_col_maint_worker_main(Datum main_arg)
{
	maint_worker(main_arg, false);
}

void
sift_col_read_promote_worker_main(Datum main_arg)
{
	maint_worker(main_arg, true);
}

void
sift_col_maint_init(void)
{
	sift_mops_register(&maint_col_ops);
}
