/*
 * col/maint/maint.c - the maintenance of a sift_col index: the entry points of a seal, a make-room,
 * a promotion, a merge step and the background workers
 * the files next to it hold what they run
 */
#include "postgres.h"
#include "core/ops.h"
#include "storage/lock.h"
#include "col/maint/maint_int.h"

/* The automatic rewrite (sift.col_auto_merge*, registered in col/am.c) */
bool		sift_col_auto_merge = true;

int			sift_col_auto_merge_dirty_pct = 30;

double		sift_col_auto_merge_fragmentation = 2.0;

int			sift_col_auto_merge_max_active = 0;

int			sift_col_auto_merge_partition_kb = 0;

int			sift_col_auto_merge_max_workers = 0;

#define PART_MIN_KB		4096	/* a partition is never smaller, whatever maintenance_work_mem is */

#define REWRITE_SLOT_KEY	0x53520000	/* with 0x51F7 in the advisory lock space: slot s of sift.col_auto_merge_max_workers is key + s */

/*
 * The size of a partition in effect for the directory in hand: sift.col_auto_merge_partition_kb, else the smaller of sift.vacuum_rewrite_limit (for an index
 * over it, so that the rewrite of a large index is a step for each partition, the lock let go between them, and not one step for all of it) and a third of
 * maintenance_work_mem: the writer of a segment keeps the key and address of each row of a global order until its last unit is in, about 2.5 bytes of memory
 * for each byte of segment. The memory bound gives way to the directory: an index never has more than PART_DIVISOR partitions. 0: no partitions.
 */
int	maint_part_kb = 0;

void
maint_use_partitions(const SiftDirSnapshot *snap)
{
	uint64		bytes = 0;
	uint64		mem_kb;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			bytes += snap->segs[i].bytes;
	maint_part_kb = sift_col_auto_merge_partition_kb > 0 ? sift_col_auto_merge_partition_kb :
		bytes > ((uint64) sift_vacuum_rewrite_limit << 20) ? sift_vacuum_rewrite_limit * 1024 : 0;
	if (sift_col_auto_merge_partition_kb > 0)
		return;
	mem_kb = Max((uint64) maintenance_work_mem / 3, Max(bytes / 1024 / PART_DIVISOR, PART_MIN_KB));
	if (bytes / 1024 > mem_kb && (maint_part_kb == 0 || mem_kb < (uint64) maint_part_kb))
		maint_part_kb = (int) Min(mem_kb, (uint64) INT_MAX);
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

/* The merges of the maintenance worker sleep like vacuum_cost_delay does (sift.maintenance_cost_delay) and keep to sift.maintenance_wal_rate; its promotions, which the writers wait for, and the rollup refresh do not */
void
maint_throttle(SiftWriteTarget *wt)
{
	if (MyBackendType == B_BG_WORKER && strcmp(MyBgworkerEntry->bgw_type, "sift maintenance") == 0)
	{
		sift_wt_throttle(wt, sift_maintenance_cost_delay);
		sift_wt_wal_rate(wt, sift_maintenance_wal_rate);
	}
}

/* The participants of the parallel build that is merging its own segments (col/build_par.c); 0: one process merges, as VACUUM and the maintenance worker do */
int	maint_merge_workers = 0;

const SiftMaintOps maint_col_ops = {
	.name = "sift_col",
	.worker = "sift_col_maint_worker_main",
	.step_lock = ExclusiveLock,
	.vacuum_to_fixed_point = true,
	.vacuum_budget = true,
	.force_seals = true,
	.vacuum_truncate = maint_vacuum_truncates,
	.vacuum_seal = sift_cwbuf_seal_vacuum,
	.is_index = maint_is_col,
	.seal = sift_cwbuf_seal,
	.promote_next = maint_promote_next,
	.merge_next = maint_merge_next,
	.bulkdelete_immutable = maint_bulkdelete_immutable,
	.bulkdelete_wbuf = maint_bulkdelete_wbuf,
	.vacuum_skipped = maint_reap_overflow,
	.rewrite_due = maint_rewrite_due,
	.rewrite_all = maint_rewrite_all,
	.compact_segment = maint_compact_segment,
	.vacuum_space_defer = maint_space_deferred,
	.vacuum_done = maint_rewrite_request,
};

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
	PG_TRY();
	{
		merged = sift_mops_rewrite(&maint_col_ops, index, false) > 0;
	}
	PG_FINALLY();
	{
		maint_merge_workers = 0;
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
 * A worker whose transaction fails keeps the error where sift.workers and the backoff of the requests find it (the steps of a sift_col index run
 * outside a subtransaction, so the whole transaction of the worker is the unit), warns as a failed BM25 step does, and ends. Cancellation and
 * shutdown go on as they were.
 */
static void
worker_failed(Oid indexoid, MemoryContext cx)
{
	ErrorData  *err;

	MemoryContextSwitchTo(cx);
	err = CopyErrorData();
	FlushErrorState();
	AbortCurrentTransaction();
	if (ERRCODE_TO_CATEGORY(err->sqlerrcode) == ERRCODE_OPERATOR_INTERVENTION)
		ReThrowError(err);
	ereport(WARNING,
			(errcode(err->sqlerrcode),
			 errmsg("maintenance of sift_col index %u failed: %s", indexoid, err->message),
			 errdetail("It is retried later, after a growing delay.")));
	sift_maint_record_failure(indexoid, err);
	FreeErrorData(err);
}

void
sift_col_maint_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;

	PG_TRY();
	{
		Relation	index;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		index = try_index_open(indexoid, RowExclusiveLock);
		if (index != NULL && sift_col_is_index(index))
		{
			/* the lock is busy when another worker or a VACUUM is at it, which then does the work */
			while (sift_col_maint_promote(index, false) > 0 || sift_col_maint_merge_step(index, false))
				CHECK_FOR_INTERRUPTS();
			sift_log_maint(index, "column maintenance worker finished", start, -1, -1);
		}
		if (index != NULL)
			index_close(index, RowExclusiveLock);
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		worker_failed(indexoid, cx);
	}
	PG_END_TRY();
}

/*
 * sift.col_auto_merge_max_workers: a worker rewrites while it holds one of that many session locks of the database (advisory locks of the keys
 * REWRITE_SLOT_KEY + slot, which the server drops when the worker ends, whatever the way), so no more than that many rewrite at a time, whichever
 * indexes they are for.
 */
static bool
take_slot(void *arg)
{
	for (int s = 0; s < sift_col_auto_merge_max_workers; s++)
	{
		LOCKTAG		tag;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, REWRITE_SLOT_KEY + s, 2);
		if (LockAcquire(&tag, ExclusiveLock, true, true) != LOCKACQUIRE_NOT_AVAIL)
			return true;
	}
	return sift_col_auto_merge_max_workers == 0;
}

/* The rewrite VACUUM asked for: the rule is judged again (the index may have changed since), and a held maintenance lock leaves it for the next VACUUM */
void
sift_col_rewrite_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;

	/* sift.col_auto_merge_max_active, then a slot of sift.col_auto_merge_max_workers; False: it stayed busy, and the next VACUUM asks again */
	if (!sift_maint_wait_quiet(sift_col_auto_merge_max_active) || !sift_nap_until(take_slot, NULL))
		return;
	PG_TRY();
	{
		Relation	index;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		index = try_index_open(indexoid, RowExclusiveLock);
		if (index != NULL)
		{
			if (sift_col_is_index(index) && sift_col_auto_merge && !RecoveryInProgress())
			{
				TimestampTz start = GetCurrentTimestamp();

				if (sift_mops_rewrite(&maint_col_ops, index, true) >= 0)
					sift_log_maint(index, "column rewrite worker finished", start, -1, -1);
			}
			index_close(index, RowExclusiveLock);
		}
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		worker_failed(indexoid, cx);
	}
	PG_END_TRY();
}
