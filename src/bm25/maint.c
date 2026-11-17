/*
 * bm25/maint.c - promotion of sealed write buffers, tiered merge, VACUUM
 * callbacks, the orphan sweep and the asynchronous maintenance worker
 *
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
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "bm25/am.h"
#include "col/pg.h"
#include "core/catname.h"
#include "core/errctx.h"
#include "core/limits.h"
#include "core/live.h"
#include "bm25/maint.h"
#include "bm25/maint_int.h"
#include "bm25/merge.h"
#include "bm25/wbuf.h"
#include "core/ops.h"
#include "core/progress.h"
#include "core/quarantine.h"
#include "core/worker.h"
#include "col/wbuf.h"
#include "core/sql.h"
#include "core/visibility.h"

#define BACKOFF_MAX_MS		3600000

#ifdef SIFT_TESTING
int			sift_test_merge_max_kb = 0;
int			sift_test_room_segments = 0;
bool		sift_test_no_heap_positions = false;
#define room_segments()		(sift_test_room_segments > 0 ? sift_test_room_segments : (int) sift_limit(max_segments, SIFT_MAX_SEGMENTS) - ROOM_SPARE)
/* longest wait of an inserter for the maintenance of others (sift._test_maint_wait_us) */
int64		sift_maint_wait_max_us = 0;
#define wait_started()		GetCurrentTimestamp()
#define wait_done(start)	(sift_maint_wait_max_us = Max(sift_maint_wait_max_us, GetCurrentTimestamp() - (start)))
#else
#define room_segments()		(SIFT_MAX_SEGMENTS - ROOM_SPARE)
#define wait_started()		((TimestampTz) 0)
#define wait_done(start)	((void) (start))
#endif
#define ROOM_SPARE			16		/* directory entries below the limit from which inserters merge */
#define ROOM_SEGMENTS		(SIFT_MAX_SEGMENTS - ROOM_SPARE)
#define SELF_PROMOTE_SEALED	4		/* sealed segments from which an inserter promotes them itself */
/* Whatever merge_fanin is, the policy merges (from SIFT_MERGE_ENTRIES entries) before inserters have to make room, and a merge fits a directory */
StaticAssertDecl(SIFT_MERGE_ENTRIES(SIFT_MERGE_MAX_FANIN) + SELF_PROMOTE_SEALED + 1 < ROOM_SEGMENTS, "merge policy acts after the room threshold");
StaticAssertDecl(2 * SIFT_MERGE_MAX_FANIN <= SIFT_MAX_SEGMENTS, "a merge of the largest merge_fanin overflows a directory");

static Oid	options_index = InvalidOid;
static TransactionId options_xid = InvalidTransactionId;

void
sift_maint_options_written(Relation index)
{
	options_index = RelationGetRelid(index);
	options_xid = GetTopTransactionId();
}

/* The lock a maintainer took after the commit of an option change reads the options of the commit, not those the cached state of its backend held while it waited */
static bool
maint_lock_mode(Relation index, bool wait, LOCKMODE mode)
{
	if (wait)
		LockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
	else if (!ConditionalLockPage(index, SIFT_MAINT_LOCK_BLKNO, mode))
		return false;
	AcceptInvalidationMessages();
	return true;
}

static void
maint_unlock_mode(Relation index, LOCKMODE mode)
{
	if (options_index == RelationGetRelid(index) && TransactionIdEquals(options_xid, GetTopTransactionIdIfAny()))
		return;
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
		/* the entries of a scope segment (col/scope.h) are replicas of rows the other segments hold: the totals of the index count the rows once */
		if (replaced && dir->segs[i].kind == SIFT_SEG_IMMUTABLE && !(dir->segs[i].flags & SIFT_SEG_SCOPE))
		{
			meta->ndocs -= dir->segs[i].ndocs;
			meta->sum_dl -= dir->segs[i].sum_dl;
		}
		if (!replaced)
			dir->segs[n++] = dir->segs[i];
	}
	if (sw->neu)
	{
		int			k = 1 + sw->nmore;

		Assert(at >= 0);

		if (n + k > SIFT_MAX_SEGMENTS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the directory has no room for the %d segments this merge wrote", k),
					 errhint("Merge the segments first (sift.merge(), or sift.col_merge() for a sift_col index), or raise sift.col_auto_merge_partition_kb so that a rewrite cuts fewer partitions.")));
		memmove(&dir->segs[at + k], &dir->segs[at], (n - at) * sizeof(SiftSegEntry));
		for (int j = 0; j < k; j++)
		{
			const SiftSegEntry *e = j == 0 ? sw->neu : &sw->more[j - 1];

			dir->segs[at + j] = *e;
			if (e->kind == SIFT_SEG_IMMUTABLE && !(e->flags & SIFT_SEG_SCOPE))
			{
				meta->ndocs += e->ndocs;
				meta->sum_dl += e->sum_dl;
			}
		}
		n += k;
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

uint64
maint_written_bytes(void)
{
	return maint_written;
}

void
sift_maint_replace_n(Relation index, const uint64 *old, int nold, const SiftSegEntry *neu, int nneu, SiftExtent *release, int nrelease,
					 const char *what, TimestampTz start, int64 docs)
{
	SiftMaintSwitch sw = {.old = old,.nold = nold,.neu = nneu > 0 ? neu : NULL,.more = nneu > 1 ? neu + 1 : NULL,.nmore = Max(nneu - 1, 0)};
	uint64		bytes = 0;

	for (int i = 0; i < nneu; i++)
		bytes += neu[i].bytes;
	maint_written += bytes;
	sift_progress_step(nold, bytes);

	sift_dir_update(index, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,.release = release,
			.nrelease = nrelease,.extra = InvalidBuffer});
	sift_log_maint(index, what, start, docs, bytes);
}

void
sift_maint_replace(Relation index, const uint64 *old, int nold, const SiftSegEntry *neu, SiftExtent *release, int nrelease,
				   const char *what, TimestampTz start, int64 docs)
{
	sift_maint_replace_n(index, old, nold, neu, neu ? 1 : 0, release, nrelease, what, start, docs);
}

int64
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

static bool make_room_locked(const SiftMaintOps *ops, Relation index);

/* Promotes every sealed segment, oldest first, until one fails; returns the number promoted */
int
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
	SiftErrCtx	ctx;
	int			n;
	bool		began;

	if (!maint_lock_mode(index, wait, ops->step_lock))
		return 0;
	sift_errctx_push(&ctx, RelationGetRelid(index));
	began = sift_progress_begin(index, SIFT_JOB_PROMOTE);
	sift_progress_phase(index, "promote");
	n = promote_locked(ops, index, false, false);
	sift_progress_end(began);
	sift_errctx_pop(&ctx);
	maint_unlock_mode(index, ops->step_lock);
	return n;
}

bool
sift_mops_make_room(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftErrCtx	ctx;
	bool		room = false;

	sift_dir_read(index, snap);
	if (!in_backoff(&snap->meta))
	{
		TimestampTz start = GetCurrentTimestamp();

		maint_lock_mode(index, true, ops->step_lock);
		wait_done(start);
		sift_errctx_push(&ctx, RelationGetRelid(index));
		room = make_room_locked(ops, index);
		sift_errctx_pop(&ctx);
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
	SiftErrCtx	ctx;
	uint64		busy = 0;
	bool		began;

	if (!maint_lock_mode(index, wait, ops->step_lock))
		return false;
	sift_errctx_push(&ctx, RelationGetRelid(index));
	began = sift_progress_begin(index, SIFT_JOB_MERGE);
	sift_progress_phase(index, "merge");
	r = ops->merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
	sift_progress_end(began);
	sift_errctx_pop(&ctx);
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

/*
 * Opens an index with its table locked first, in the order of a statement: DROP TABLE and the commands like it lock the table and then its indexes, so a backend that holds an
 * index and then asks for the table waits for a command that waits for the index. The locks are held to the end of the transaction; NULL when the index is gone (missing_ok).
 * An index that its creating transaction had not committed when the table was looked up is opened after that commit, with no table lock: it is closed, and the table it
 * belongs to is locked on the next turn.
 */
static Relation
open_index_table_first(Oid indexoid, LOCKMODE lock, bool missing_ok)
{
	for (;;)
	{
		Oid			heapoid = IndexGetRelation(indexoid, true);
		Relation	index;

		SIFT_POINT("sift-maint-before-table-lock");
		if (OidIsValid(heapoid))
			LockRelationOid(heapoid, lock);
		SIFT_POINT("sift-maint-after-table-lock");
		index = try_index_open(indexoid, lock);
		if (index == NULL && !missing_ok)
			sift_rel_missing(indexoid);
		if (index == NULL || index->rd_index->indrelid == heapoid)
			return index;
		index_close(index, lock);
	}
}

/* What a worker or a transaction that records a failure opens: NULL when the index is gone */
Relation
sift_try_open_index(Oid indexoid, LOCKMODE lock)
{
	return open_index_table_first(indexoid, lock, true);
}

Relation
sift_mops_open(const SiftMaintOps *ops, Oid indexoid, LOCKMODE lock)
{
	Relation	index = open_index_table_first(indexoid, lock, false);

	if (!ops->is_index(index))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index", RelationGetRelationName(index), ops->name)));
	sift_store_require_ready(index);
	return index;
}

Relation
sift_mops_open_maintain(const SiftMaintOps *ops, Oid indexoid)
{
	Relation	index = sift_mops_open(ops, indexoid, RowExclusiveLock);

	sift_check_maintain(index);
	return index;
}

bool		sift_maint_own_xacts = false;

#define FREED_WAIT_MS		10
#define FREED_WAIT_TRIES	50

/*
 * The end of a step of a background worker: its transaction commits and the next step runs in a new one. What a step frees carries its transaction's xid and the
 * worker's snapshot holds the horizon at that xid for as long as the transaction lasts, so a step could not reuse what the one before freed (the file grew by
 * the size of the output of every step of a rewrite, and the next VACUUM cut it back). NULL when the index is gone.
 */
Relation
sift_maint_step_boundary(Relation index)
{
	Oid			indexoid = RelationGetRelid(index);

	if (!sift_maint_own_xacts)
		return index;
	index_close(index, RowExclusiveLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL && sift_index_known_out(index))
	{
		index_close(index, RowExclusiveLock);
		return NULL;
	}
	/* a read that began while the step ran ends within moments; one that outlasts the wait holds the blocks of every step of the pass, which then writes where it can */
	for (int i = 0; index != NULL && i < FREED_WAIT_TRIES && sift_store_held_blocks(index) > 0; i++)
		sift_nap(FREED_WAIT_MS);
	return index;
}

/* Seals the active segment and promotes every sealed one; returns the number promoted */
void
sift_mops_cut_free_end(const SiftMaintOps *ops, Relation index)
{
	cut_free_end(ops, index, false);
}

int
sift_mops_rewrite(const SiftMaintOps *ops, Relation *indexp, bool automatic)
{
	Relation	index = *indexp;
	const char *why;
	TimestampTz start = GetCurrentTimestamp();
	SiftErrCtx	ctx;
	bool		all,
				began;
	int			n = 0;

	if (!maint_lock_mode(index, false, ops->step_lock))
		return -1;
	sift_errctx_push(&ctx, RelationGetRelid(index));
	if ((why = ops->rewrite_due(index, automatic)) == NULL)
	{
		sift_errctx_pop(&ctx);
		maint_unlock_mode(index, ops->step_lock);
		return -1;
	}
	all = ops->rewrite_all == NULL || ops->rewrite_all(why);
	began = sift_progress_begin(index, SIFT_JOB_REWRITE);
	sift_progress_phase(index, "promote");
	promote_locked(ops, index, true, true);
	sift_progress_phase(index, "merge");
	/* the lock is let go between the steps, so that VACUUM and the writers wait for one partition of the rewrite and not for all of it; the rewrite waits for it in turn */
	for (;;)
	{
		uint64		busy = 0;
		bool		more = ops->merge_next(index, false, all ? SIFT_GOAL_REWRITE : SIFT_GOAL_ANY, true, &busy) == SIFT_STEP_DONE;

		sift_errctx_pop(&ctx);
		maint_unlock_mode(index, ops->step_lock);
		if (!more)
			break;
		n++;
		CHECK_FOR_INTERRUPTS();
		/* a step of the kind makes progress; this is the end of a kind that does not */
		if (n >= SIFT_MAX_SEGMENTS)
			break;
		*indexp = index = sift_maint_step_boundary(index);
		if (index == NULL)
		{
			sift_progress_end(began);
			return n;
		}
		maint_lock_mode(index, true, ops->step_lock);
		sift_errctx_push(&ctx, RelationGetRelid(index));
	}
	if (n > 0)
	{
		sift_progress_phase(index, "space");
		cut_free_end(ops, index, true);
	}
	sift_progress_end(began);
	sift_log_maint(index, psprintf(all ? "rewrote (%s) the segments of" : "promoted (%s) the write buffer of", why), start, -1, -1);
	return n;
}

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
	.vacuum_seal = sift_wbuf_seal_vacuum,
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

static void
stamp_request(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->maint_requested_at = *(TimestampTz *) arg;
}

/* By oid: what a commit callback has, where no relation can be opened */
bool
sift_maint_register_worker_oid(Oid indexoid, const char *function, const char *subject)
{
	return sift_worker_register(function, subject, ObjectIdGetDatum(indexoid));
}

bool
sift_maint_register_worker(Relation index, const char *function)
{
	return sift_maint_register_worker_oid(RelationGetRelid(index), function, sift_worker_subject(index));
}

bool
sift_maint_request_worker(Relation index, const char *function)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TimestampTz now = GetCurrentTimestamp();
	bool		started;

	sift_dir_read(index, snap);
	started = now - snap->meta.maint_requested_at >= backoff_ms(&snap->meta) * INT64CONST(1000) && !sift_index_out(index, snap->meta.flags);
	pfree(snap);
	if (!started)
		return false;
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_request,.arg = &now,.extra = InvalidBuffer});
	return sift_maint_register_worker(index, function);
}

bool
sift_maint_busy(int max_active)
{
	int			n = 0;

	if (max_active <= 0)
		return false;
	pgstat_clear_backend_activity_snapshot();
	for (int i = 1, max = pgstat_fetch_stat_numbackends(); i <= max; i++)
	{
		const PgBackendStatus *s = &pgstat_get_local_beentry_by_index(i)->backendStatus;

		n += s->st_backendType == B_BACKEND && s->st_databaseid == MyDatabaseId && s->st_state == STATE_RUNNING;
	}
	return n > max_active;
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
	sift_worker_announce();
	return indexoid;
}

/*
 * What a document of the open write buffer costs each query that walks it (its page passed the bloom filter, which a 256-bit bloom on 900 and more terms a page lets nearly all
 * pages do: 170 cycles for an absent term, 440 for a term of 5% of the documents, against 80 once it is in a segment) and what the promotion costs, once (55,000 cycles a
 * document: 295 M cycles for 5,000, 95 ms with the WAL). Cycles, measured on 200,000 documents with a 5,000-document buffer.
 */
#define WBUF_WALK_PRICE			250.0
#define WBUF_PROMOTE_PRICE		55000.0
/* a maintenance step counts at half, and a buffer under this many documents is not worth a segment (a segment costs every later query about 140,000 cycles, the walk of 600 documents) */
#define WBUF_PROMOTE_HALF		0.5
#define WBUF_READ_MIN_DOCS		2000

/*
 * Have the queries that walked the open write buffer paid for its promotion? A buffer nobody reads has no walks noted, so a table that is only written never promotes before
 * its limit, and the sum is per generation of the buffer, so one promotion ends the debt.
 */
bool
sift_maint_wbuf_read_due(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		due = false;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs && !due; i++)
	{
		double		paid;
		uint64		ndocs,
					sum_dl,
					npages;

		if (snap->segs[i].kind != SIFT_SEG_WBUF_OPEN)
			continue;
		paid = (double) sift_cwbuf_decoded(index, &snap->meta, snap->segs[i].gen) * WBUF_WALK_PRICE;
		if (paid < WBUF_PROMOTE_HALF * WBUF_PROMOTE_PRICE * WBUF_READ_MIN_DOCS)
			continue;
		sift_wbuf_summary(index, &snap->segs[i], &ndocs, &sum_dl, &npages);
		due = ndocs >= WBUF_READ_MIN_DOCS && paid >= WBUF_PROMOTE_HALF * WBUF_PROMOTE_PRICE * (double) ndocs;
	}
	pfree(snap);
	return due;
}

/* A search walked docs documents of the open buffer of generation gen: note them, write the sum to the metapage (col/wbuf/reads.c), and now and then ask whether the reads have paid for the promotion (at most once per sift.maintenance_naptime for a backend) */
void
sift_maint_wbuf_walked(Relation index, uint64 gen, uint64 docs)
{
	static Oid	looked = InvalidOid;
	static TimestampTz looked_at = 0;
	TimestampTz now;

	sift_cwbuf_decoded_note(index, gen, docs);
	sift_reads_flush(index, sift_maint_wbuf_read_due, NULL);
	if (!sift_async_maintenance || !IsUnderPostmaster || IsParallelWorker() || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	now = GetCurrentTimestamp();
	if (looked == RelationGetRelid(index) && now - looked_at < (int64) sift_maintenance_naptime * 1000)
		return;
	looked = RelationGetRelid(index);
	looked_at = now;
	if (sift_maint_wbuf_read_due(index))
		sift_maint_request_worker(index, "sift_read_promote_worker_main");
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
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		/* the lock is busy when a VACUUM is at it, which then does the work */
		if (is_sift(index) && ConditionalLockPage(index, SIFT_WORKER_LOCK_BLKNO, ExclusiveLock) &&
			maint_lock_mode(index, false, RowExclusiveLock))
		{
			uint64		busy = 0;
			TimestampTz began = GetCurrentTimestamp();
			SiftJob		job = SIFT_JOB_PROMOTE;

			if ((r = promote_next(index, false, &busy)) == SIFT_STEP_IDLE)
			{
				job = SIFT_JOB_MERGE;
				r = merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
			}
			maint_unlock_mode(index, RowExclusiveLock);
			if (r == SIFT_STEP_DONE)
				sift_worker_ok(index, job, began);
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
 * of the request that started it asks for no worker of its own, and its segments would wait for the next seal or a VACUUM. So
 * does a look that finds the maintenance lock with an inserter, which promotes and leaves the merges to the worker.
 * The last look is made after the window ended, so every such seal is before it.
 */
void
sift_maint_worker_stay(Oid indexoid, TimestampTz start, SiftWorkerStep step)
{
	for (;;)
	{
		long		left = sift_maintenance_naptime - TimestampDifferenceMilliseconds(start, GetCurrentTimestamp());
		SiftStepResult	r;

		while ((r = step(indexoid, start, left <= 0)) == SIFT_STEP_DONE)
			CHECK_FOR_INTERRUPTS();
		if (r == SIFT_STEP_FAILED || left <= 0)
			break;
		sift_worker_nap(Min(left, Min(Max(sift_maintenance_naptime / 20, 100), 1000)));
	}
}

static void
worker_main(Datum main_arg, bool seal)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);

	if (seal)
	{
		Relation	index;

		StartTransactionCommand();
		index = sift_try_open_index(indexoid, RowExclusiveLock);
		if (index != NULL)
		{
			/* judged again: the buffer may have been promoted since the reads asked */
			if (is_sift(index) && sift_maint_wbuf_read_due(index))
				sift_wbuf_seal(index, true);
			index_close(index, RowExclusiveLock);
		}
		CommitTransactionCommand();
	}
	sift_maint_worker_stay(indexoid, start, worker_step);
}

void
sift_maint_worker_main(Datum main_arg)
{
	worker_main(main_arg, false);
}

/* The worker that seals the open write buffer first, as the reads that walked it asked */
void
sift_read_promote_worker_main(Datum main_arg)
{
	worker_main(main_arg, true);
}

Datum
sift_promote(PG_FUNCTION_ARGS)
{
	if (sift_col_is_index_oid(PG_GETARG_OID(0)))
		return sift_col_promote(fcinfo);
	return sift_mops_sql_promote(&bm25_ops, fcinfo);
}

Datum
sift_merge(PG_FUNCTION_ARGS)
{
	if (sift_col_is_index_oid(PG_GETARG_OID(0)))
		return sift_col_merge(fcinfo);
	return sift_mops_sql_merge(&bm25_ops, fcinfo);
}
