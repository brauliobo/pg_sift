/*
 * bm25/maint.c - promotion of sealed write buffers, tiered merge, VACUUM
 * callbacks, the orphan sweep and the asynchronous maintenance worker
 * (DESIGN.md (c), (i)).
 *
 * Promotion and merge steps hold the maintenance lock (LockPage SIFT_MAINT_LOCK_BLKNO) in
 * RowExclusiveLock, which steps share, and claim the segments they replace, one heavyweight lock per
 * generation, so the steps of the worker and of inserters on other segments run side by side and a
 * long merge stalls nobody. sift.verify() holds it in ShareLock, which keeps the steps out; VACUUM,
 * the orphan sweep and the SQL functions hold it exclusive: nothing else is then at work on the index.
 * A claimed segment can only be changed by its holder. Readers are safe through the store's
 * safexid protocol (core/store.h P4). Promotion and merge steps are in step.c; the orphan sweep,
 * truncation and the space step in space.c.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"
#include "utils/tuplesort.h"
#include "utils/timestamp.h"

#include "bm25/am.h"
#include "bm25/build.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/maint_int.h"
#include "bm25/merge.h"
#include "bm25/wbuf.h"
#include "core/ops.h"
#include "core/sql.h"
#include "core/visibility.h"

#define BACKOFF_MAX_MS		3600000

#ifdef SIFT_TESTING
int			sift_test_merge_max_kb = 0;
int			sift_test_merge_sleep_ms = 0;
int			sift_test_room_segments = 0;
#define room_segments()		(sift_test_room_segments > 0 ? sift_test_room_segments : ROOM_SEGMENTS)
/* longest wait of an inserter for the maintenance of others (sift._test_maint_wait_us) */
int64		sift_maint_wait_max_us = 0;
#define wait_started()		GetCurrentTimestamp()
#define wait_done(start)	(sift_maint_wait_max_us = Max(sift_maint_wait_max_us, GetCurrentTimestamp() - (start)))
#else
#define room_segments()		ROOM_SEGMENTS
#define wait_started()		((TimestampTz) 0)
#define wait_done(start)	((void) (start))
#endif
#define ROOM_SEGMENTS		(SIFT_MAX_SEGMENTS - 16)	/* directory entries from which inserters merge */
#define SELF_PROMOTE_SEALED	4		/* sealed segments from which an inserter promotes them itself */
/* Whatever merge_fanin is, the policy merges (from SIFT_MERGE_ENTRIES entries) before inserters have to make room, and a merge fits a directory */
StaticAssertDecl(SIFT_MERGE_ENTRIES(SIFT_MERGE_MAX_FANIN) + SELF_PROMOTE_SEALED + 1 < ROOM_SEGMENTS, "merge policy acts after the room threshold");
StaticAssertDecl(2 * SIFT_MERGE_MAX_FANIN <= SIFT_MAX_SEGMENTS, "a merge of the largest merge_fanin overflows a directory");

/* ---------- locking and directory helpers ---------- */

static bool
maint_lock_mode(Relation index, bool wait, LOCKMODE mode)
{
	if (wait)
	{
		LockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
		return true;
	}
	return ConditionalLockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
}

static void
maint_unlock_mode(Relation index, LOCKMODE mode)
{
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
}

bool
sift_maint_lock(Relation index, bool wait)
{
	return maint_lock_mode(index, wait, ExclusiveLock);
}

void
sift_maint_unlock(Relation index)
{
	maint_unlock_mode(index, ExclusiveLock);
}

/* Waits until the holder of the claim is done, which is how an inserter waits for a full directory to shrink */
static void
wait_claim(Relation index, uint64 gen)
{
	TimestampTz start = wait_started();

	claim(index, gen, true);
	unclaim(index, gen);
	wait_done(start);
}

/* ---------- switching segments in the directory ---------- */

void
sift_maint_switch_segments(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SiftMaintSwitch *sw = arg;
	int			n = 0;
	int			at = -1;

	for (int i = 0; i < dir->nsegs; i++)
	{
		bool		replaced = false;

		for (int k = 0; k < sw->nold; k++)
			replaced |= dir->segs[i].gen == sw->old[k];
		if (replaced && at < 0)
			at = n;
		if (replaced && dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			meta->ndocs -= dir->segs[i].ndocs;
			meta->sum_dl -= dir->segs[i].sum_dl;
		}
		if (!replaced)
			dir->segs[n++] = dir->segs[i];
	}
	if (sw->neu)
	{
		memmove(&dir->segs[at + 1], &dir->segs[at], (n - at) * sizeof(SiftSegEntry));
		dir->segs[at] = *sw->neu;
		n++;
		if (sw->neu->kind == SIFT_SEG_IMMUTABLE)
		{
			meta->ndocs += sw->neu->ndocs;
			meta->sum_dl += sw->neu->sum_dl;
		}
	}
	dir->nsegs = n;
	meta->flags &= ~SIFT_META_FAILURES_MASK;
}

/* The generations and extents of in[0..n), allocated in the current memory context */
SiftMaintInputs
sift_maint_inputs(Relation index, const SiftSegEntry *in, int n)
{
	SiftMaintInputs mi = {.gens = palloc(n * sizeof(uint64)),.release = palloc(0)};

	for (int i = 0; i < n; i++)
	{
		int			k;
		const SiftExtent *ext = sift_store_immutable_extents(index, &in[i], &k);

		mi.release = repalloc(mi.release, (mi.nrelease + k) * sizeof(SiftExtent));
		memcpy(mi.release + mi.nrelease, ext, k * sizeof(SiftExtent));
		mi.nrelease += k;
		mi.gens[i] = in[i].gen;
		mi.bytes += in[i].bytes;
	}
	return mi;
}

/* The bytes of the segments the steps of this backend wrote (VACUUM budgets its merges by what they add to it) */
static uint64 maint_written = 0;

void
sift_maint_replace(Relation index, const uint64 *old, int nold, const SiftSegEntry *neu, SiftExtent *release, int nrelease,
				   const char *what, TimestampTz start, int64 docs)
{
	SiftMaintSwitch sw = {.old = old,.nold = nold,.neu = neu};

	if (neu)
		maint_written += neu->bytes;

	sift_dir_update(index, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,.release = release,
			.nrelease = nrelease,.extra = InvalidBuffer});
	sift_log_maint(index, what, start, docs, neu ? neu->bytes : 0);
}

static int64
backoff_ms(const SiftMetaPageData *meta)
{
	uint32		failures = meta->flags & SIFT_META_FAILURES_MASK;

	return Min((int64) sift_maintenance_naptime << Min(failures, 20), BACKOFF_MAX_MS);
}

/* Failed steps double the time before the next one is attempted in the background, up to an hour */
bool
in_backoff(const SiftMetaPageData *meta)
{
	return (meta->flags & SIFT_META_FAILURES_MASK) != 0 &&
		GetCurrentTimestamp() - meta->maint_requested_at < backoff_ms(meta) * INT64CONST(1000);
}

/* ---------- the steps of an index kind: the control flow shared by BM25 and sift_col ---------- */

static bool make_room_locked(const SiftMaintOps *ops, Relation index);

/* Promotes every sealed segment, oldest first, until one fails; returns the number promoted */
static int
promote_locked(const SiftMaintOps *ops, Relation index, bool seal_active, bool propagate)
{
	uint64		busy = 0;
	int			n = 0;

	if (seal_active)
	{
		make_room_locked(ops, index);
		ops->seal(index, true);
	}
	while (ops->promote_next(index, propagate, &busy) == SIFT_STEP_DONE)
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	return n;
}

/*
 * Promotes and merges until the directory is below ROOM_SEGMENTS entries, so a seal always finds a
 * free one; false when it cannot (nothing to merge, or a step failed).
 */
static bool
make_room_locked(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room;

	for (;;)
	{
		SiftStepResult	r;
		uint64		busy = 0;

		sift_dir_read(index, snap);
		if (snap->nsegs < room_segments())
			break;
		if ((r = ops->promote_next(index, false, &busy)) == SIFT_STEP_IDLE)
			r = ops->merge_next(index, false, SIFT_GOAL_ANY, false, &busy);
		if (r == SIFT_STEP_IDLE && busy != 0)
		{
			/* what is left to merge is claimed by others: their step frees the entries */
			wait_claim(index, busy);
			continue;
		}
		if (r != SIFT_STEP_DONE)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	room = snap->nsegs < room_segments();
	pfree(snap);
	return room;
}

int
sift_mops_promote(const SiftMaintOps *ops, Relation index, bool wait)
{
	int			n;

	if (!maint_lock_mode(index, wait, ops->step_lock))
		return 0;
	n = promote_locked(ops, index, false, false);
	maint_unlock_mode(index, ops->step_lock);
	return n;
}

bool
sift_mops_make_room(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room = false;

	sift_dir_read(index, snap);
	if (!in_backoff(&snap->meta))
	{
		TimestampTz start = GetCurrentTimestamp();

		maint_lock_mode(index, true, ops->step_lock);
		wait_done(start);
		room = make_room_locked(ops, index);
		maint_unlock_mode(index, ops->step_lock);
		sift_log_stall(index, start);
	}
	pfree(snap);
	return room;
}

bool
sift_mops_merge_step(const SiftMaintOps *ops, Relation index, bool wait)
{
	SiftStepResult	r;
	uint64		busy = 0;

	if (!maint_lock_mode(index, wait, ops->step_lock))
		return false;
	r = ops->merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
	maint_unlock_mode(index, ops->step_lock);
	return r == SIFT_STEP_DONE;
}

/*
 * Promotion is asynchronous when allowed. Back-pressure: with many sealed
 * segments waiting, the inserter promotes itself unless maintenance is busy,
 * and with the directory nearly full it merges itself, waiting for the
 * maintenance lock (inserts slow down instead of failing).
 */
void
sift_mops_after_seal(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int			nsealed = 0;

	sift_dir_read(index, snap);
	if (snap->nsegs >= room_segments())
		sift_mops_make_room(ops, index);
	if (ops->seal(index, false) == 0)
	{
		pfree(snap);
		return;
	}
	if (sift_async_maintenance)
		sift_maint_request_worker(index, ops->worker);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		nsealed += snap->segs[i].kind == SIFT_SEG_WBUF_SEALED;
	if (nsealed >= SELF_PROMOTE_SEALED && !in_backoff(&snap->meta))
		sift_mops_promote(ops, index, false);
	pfree(snap);
}

/* ---------- VACUUM ---------- */

IndexBulkDeleteResult *
sift_mops_bulkdelete(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
					 void *callback_state)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64		removed;

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	removed = stats->tuples_removed;
	sift_maint_lock(index, true);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			ops->bulkdelete_immutable(index, &snap->segs[i], callback, callback_state, stats);
		else
			ops->bulkdelete_wbuf(index, &snap->segs[i], callback, callback_state, stats);
	/* The heap marks pages all-visible only after this returns: a scan that reads the map after that finds the version moved (sift_vis_begin) */
	if (stats->tuples_removed > removed)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	sift_maint_unlock(index);
	pfree(snap);
	return stats;
}

/* The merges of VACUUM: until the policy asks for none, as a build that left many segments needs, or just one; stops at a failed step */
static void
vacuum_merge(const SiftMaintOps *ops, Relation index)
{
	uint64		busy = 0;
	uint64		start = maint_written;
	uint64		limit = (uint64) sift_vacuum_rewrite_limit << 20;

	while (ops->merge_next(index, false, SIFT_GOAL_VACUUM, false, &busy) == SIFT_STEP_DONE && ops->vacuum_to_fixed_point)
	{
		CHECK_FOR_INTERRUPTS();
		if (ops->vacuum_budget && maint_written - start >= limit)
			break;
	}
}

IndexBulkDeleteResult *
sift_mops_vacuumcleanup(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		skipped = stats == NULL;

	if (info->analyze_only)
		return stats;
	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	sift_maint_lock(index, true);
	if (ops->vacuum_seal)
		ops->vacuum_seal(index);
	promote_locked(ops, index, false, false);
	if (skipped && ops->vacuum_skipped)
		ops->vacuum_skipped(index, info->heaprel);
	vacuum_merge(ops, index);
	SIFT_TEST_GATE(sift_test_cleanup_gate, 2);
	if (ops->vacuum_refresh)
		ops->vacuum_refresh(index);
	sift_recycle(index);
	sift_maint_orphan_sweep(index);
	if (ops->vacuum_truncate && ops->vacuum_truncate(index))
		sift_maint_space(index, NULL, info->message_level);
	sift_maint_unlock(index);
	sift_dir_read(index, snap);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = false;
	if (stats->num_index_tuples == 0)
		for (int i = 0; i < snap->nsegs; i++)
			stats->num_index_tuples += snap->segs[i].ndocs;
	pfree(snap);
	return stats;
}

/* ---------- SQL ---------- */

Relation
sift_mops_open(const SiftMaintOps *ops, Oid indexoid, LOCKMODE lock)
{
	Relation	index = index_open(indexoid, lock);

	if (!ops->is_index(index))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index", RelationGetRelationName(index), ops->name)));
	return index;
}

Relation
sift_mops_open_maintain(const SiftMaintOps *ops, Oid indexoid)
{
	Relation	index = sift_mops_open(ops, indexoid, RowExclusiveLock);

	sift_check_maintain(index);
	return index;
}

/* Seals the active segment and promotes every sealed one; returns the number promoted */
int
sift_mops_sql_promote(const SiftMaintOps *ops, Relation index)
{
	int			n;

	sift_maint_lock(index, true);
	n = promote_locked(ops, index, true, true);
	sift_maint_unlock(index);
	return n;
}

/* Runs merge steps until the policy asks for none; force merges all immutable segments into one */
int
sift_mops_sql_merge(const SiftMaintOps *ops, Relation index, bool force)
{
	uint64		busy = 0;
	int			n = 0;

	sift_maint_lock(index, true);
	promote_locked(ops, index, false, true);
	while (ops->merge_next(index, force && n == 0, SIFT_GOAL_ANY, true, &busy) == SIFT_STEP_DONE)
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	sift_maint_unlock(index);
	return n;
}

/* ---------- BM25 ---------- */

static bool
is_sift(Relation index)
{
	return index->rd_rel->relam == sift_catalog()->am;
}

static void
bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftSegment *seg = sift_seg_open(index, e, CurrentMemoryContext);

	sift_seg_bulkdelete(seg, cb, cb_state, stats);
	sift_seg_close(seg);
}

static void
bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	sift_wbuf_bulkdelete(index, e, cb, cb_state, stats);
}

static bool
vacuum_truncates(Relation index)
{
	return sift_index_options(index)->vacuum_truncate;
}

/* The dead-posting tables of the segments VACUUM cleared docs of and the merges left as they are */
static void
refresh_dead_tables(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_DIRTY))
		{
			SiftSegment *seg = sift_seg_open(index, &snap->segs[i], CurrentMemoryContext);

			sift_seg_refresh_dead(seg);
			sift_seg_close(seg);
		}
	pfree(snap);
}

static const SiftMaintOps bm25_ops = {
	.name = "sift",
	.worker = "sift_maint_worker_main",
	.step_lock = RowExclusiveLock,
	.vacuum_to_fixed_point = true,
	.is_index = is_sift,
	.seal = sift_wbuf_seal,
	.promote_next = promote_next,
	.merge_next = merge_next,
	.bulkdelete_immutable = bulkdelete_immutable,
	.bulkdelete_wbuf = bulkdelete_wbuf,
	.vacuum_truncate = vacuum_truncates,
	.vacuum_refresh = refresh_dead_tables,
};

void
sift_maint_after_seal(Relation index)
{
	sift_mops_after_seal(&bm25_ops, index);
}

bool
sift_maint_make_room(Relation index)
{
	return sift_mops_make_room(&bm25_ops, index);
}

IndexBulkDeleteResult *
sift_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	return sift_mops_bulkdelete(&bm25_ops, info, stats, callback, callback_state);
}

IndexBulkDeleteResult *
sift_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return sift_mops_vacuumcleanup(&bm25_ops, info, stats);
}

/* ---------- asynchronous maintenance ---------- */

static void
stamp_request(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->maint_requested_at = *(TimestampTz *) arg;
}

bool
sift_maint_request_worker(Relation index, const char *function)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TimestampTz now = GetCurrentTimestamp();
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	bool		started;

	sift_dir_read(index, snap);
	started = now - snap->meta.maint_requested_at >= backoff_ms(&snap->meta) * INT64CONST(1000);
	pfree(snap);
	if (!started)
		return false;
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_request,.arg = &now,.extra = InvalidBuffer});
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "%s", function);
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift maintenance (index %u)", RelationGetRelid(index));
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift maintenance");
	bw.bgw_main_arg = ObjectIdGetDatum(RelationGetRelid(index));
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	if (RegisterDynamicBackgroundWorker(&bw, &handle))
		return true;
	sift_log_refused(index);
	return false;
}

Oid
sift_maint_worker_connect(Datum main_arg, ErrorContextCallback *errctx)
{
	Oid			indexoid = DatumGetObjectId(main_arg);
	Oid			dboid;

	sift_maint_context(errctx, indexoid);
	memcpy(&dboid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	return indexoid;
}

/*
 * One promotion or merge in a transaction of its own, holding no snapshot: the extents it frees are
 * reusable by the next step, because neither its xid nor its xmin hold the horizon back once it
 * committed. The worker lock makes it the only worker of the index at work; the others leave.
 */
static SiftStepResult
worker_step(Oid indexoid, TimestampTz start, bool last)
{
	Relation	index;
	SiftStepResult	r = SIFT_STEP_IDLE;

	StartTransactionCommand();
	index = try_index_open(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		/* the lock is busy when a VACUUM is at it, which then does the work */
		if (is_sift(index) && ConditionalLockPage(index, SIFT_WORKER_LOCK_BLKNO, ExclusiveLock) &&
			maint_lock_mode(index, false, RowExclusiveLock))
		{
			uint64		busy = 0;

			if ((r = promote_next(index, false, &busy)) == SIFT_STEP_IDLE)
				r = merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
			maint_unlock_mode(index, RowExclusiveLock);
		}
		if (last || r == SIFT_STEP_FAILED)
			sift_log_maint(index, "maintenance worker finished", start, -1, -1);
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
	return r;
}

/*
 * The worker stays until sift.maintenance_naptime after it started, looking for work every so often: a seal inside that window
 * of the request that started it asks for no worker of its own, and its segments would wait for the next seal or a VACUUM.
 * The last look is made after the window ended, so every such seal is before it.
 */
void
sift_maint_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);

	for (;;)
	{
		long		left = sift_maintenance_naptime - TimestampDifferenceMilliseconds(start, GetCurrentTimestamp());
		SiftStepResult	r;

		while ((r = worker_step(indexoid, start, left <= 0)) == SIFT_STEP_DONE)
			CHECK_FOR_INTERRUPTS();
		if (r == SIFT_STEP_FAILED || left <= 0)
			break;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 Min(left, Min(Max(sift_maintenance_naptime / 20, 100), 1000)), PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---------- SQL ---------- */

Datum
sift_promote(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&bm25_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_promote(&bm25_ops, index);

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_merge(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&bm25_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_merge(&bm25_ops, index, PG_NARGS() > 1 && PG_GETARG_BOOL(1));

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}
