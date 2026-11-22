/*
 * core/maint.c - the maintenance every kind of index shares (SiftMaintOps): promotion of sealed write buffers, tiered merge, VACUUM callbacks, the orphan sweep and the
 * asynchronous maintenance worker
 *
 * Promotion and merge steps hold the maintenance lock (LockPage SIFT_MAINT_LOCK_BLKNO) in the mode the kind says (step_lock). A kind whose steps hold it in
 * RowExclusiveLock, which steps share, claims the segments they replace, one heavyweight lock per generation, so the steps of the worker and of inserters on other segments
 * run side by side and a long merge stalls nobody. sift.verify() holds it in ShareLock, which keeps the steps out; VACUUM, the orphan sweep and the SQL functions hold it
 * exclusive: nothing else is then at work on the index. A claimed segment can only be changed by its holder. Readers are safe through the store's safexid protocol
 * (core/store.h P4). The steps of a kind are in bm25/step.c and col/v1/maint/; the orphan sweep, truncation and the space step in core/space.c.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "core/u64set.h"
#include "core/catname.h"
#include "core/errctx.h"
#include "core/inspect.h"
#include "core/limits.h"
#include "core/live.h"
#include "core/maint.h"
#include "core/maint_int.h"
#include "core/maint_pacer.h"
#include "core/ops.h"
#include "core/progress.h"
#include "core/quarantine.h"
#include "core/worker.h"
#include "core/sql.h"
#include "core/visibility.h"
#include "core/memtrack.h"

#define BACKOFF_MAX_MS		3600000

#ifdef SIFT_TESTING
int			sift_test_merge_max_kb = 0;
int			sift_test_room_segments = 0;
bool		sift_test_no_heap_positions = false;
#define room_segments(snap)	(sift_test_room_segments > 0 ? sift_test_room_segments : sift_dir_cap(&(snap)->meta) - ROOM_SPARE)
/* longest wait of an inserter for the maintenance of others (sift._test_maint_wait_us) */
int64		sift_maint_wait_max_us = 0;
#define wait_started()		GetCurrentTimestamp()
#define wait_done(start)	(sift_maint_wait_max_us = Max(sift_maint_wait_max_us, GetCurrentTimestamp() - (start)))
#else
#define room_segments(snap)	(sift_dir_cap(&(snap)->meta) - ROOM_SPARE)
#define wait_started()		((TimestampTz) 0)
#define wait_done(start)	((void) (start))
#endif
static Oid	options_index = InvalidOid;
static TransactionId options_xid = InvalidTransactionId;

void
sift_maint_options_written(Relation index)
{
	options_index = RelationGetRelid(index);
	options_xid = GetTopTransactionId();
}

/* The lock a maintainer took after the commit of an option change reads the options of the commit, not those the cached state of its backend held while it waited */
bool
maint_lock_mode(Relation index, bool wait, LOCKMODE mode)
{
	if (wait)
		LockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
	else if (!ConditionalLockPage(index, SIFT_MAINT_LOCK_BLKNO, mode))
		return false;
	AcceptInvalidationMessages();
	return true;
}

void
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
	SiftU64Set	old;
	int			n = 0;
	int			at = -1;

	sift_u64set_init(&old, sw->nold);
	for (int k = 0; k < sw->nold; k++)
		(void) sift_u64set_add(&old, sw->old[k]);
	for (int i = 0; i < dir->nsegs; i++)
	{
		bool		replaced = sift_u64set_has(&old, dir->segs[i].gen);

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

		if (n + k > SIFT_DIR_WIDE_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the directory has no room for the %d segments this merge wrote", k),
					 errhint("Merge the segments first (sift.merge(), or sift.col_merge() for a sift_col index), or raise maintenance_work_mem so that a rewrite cuts fewer partitions.")));
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
		if (sw->part_n > 0)
			sift_dir_part_replace(sw->old, sw->nold, &dir->segs[at + sw->part_first], sw->part_n, sw->part_lo);
		else
			sift_dir_part_inherit(sw->old, sw->nold, &dir->segs[at], k);
	}
	sift_u64set_free(&old);
	dir->nsegs = n;
	meta->inserted += sw->inserted;
	meta->flags &= ~SIFT_META_FAILURES_MASK;
}

/* The generations and extents of in[0..n), allocated in the current memory context */
SiftMaintInputs
sift_maint_inputs(Relation index, const SiftSegEntry *in, int n)
{
	SiftMaintInputs mi = {.gens = palloc(n * sizeof(uint64)),.release = palloc(0)};
	int			cap = 0;

	for (int i = 0; i < n; i++)
	{
		int			k;
		const SiftExtent *ext = sift_store_immutable_extents(index, &in[i], &k);

		if (mi.nrelease + k > cap)
		{
			/* check-memory: allow the extents of the segments a maintenance step reads, which the step's inputs bound */
			cap = Max(Max(16, 2 * cap), mi.nrelease + k);
			mi.release = repalloc(mi.release, cap * sizeof(SiftExtent));
		}
		memcpy(mi.release + mi.nrelease, ext, k * sizeof(SiftExtent));
		mi.nrelease += k;
		mi.gens[i] = in[i].gen;
		mi.bytes += in[i].bytes;
	}
	return mi;
}

static const SiftMaintOps *kinds[4];
static int	nkinds;

void
sift_mops_register(const SiftMaintOps *ops)
{
	SIFT_INVARIANT(nkinds < lengthof(kinds), "sift: too many kinds of maintenance");
	kinds[nkinds++] = ops;
}

const SiftMaintOps *
sift_mops_of(Relation index)
{
	for (int i = 0; i < nkinds; i++)
		if (kinds[i]->is_index(index))
			return kinds[i];
	return NULL;
}

/* The bytes of the segments the steps of this backend wrote (VACUUM budgets its merges by what they add to it) */
static uint64 maint_written = 0;

uint64
maint_written_bytes(void)
{
	return maint_written;
}

static void
replace_n(Relation index, SiftWriteTarget *wt, const uint64 *old, int nold, const SiftSegEntry *neu, int nneu, SiftExtent *release, int nrelease,
		  const char *what, TimestampTz start, int64 docs, uint32 inserted, int part_first, int part_n, const uint8 (*part_lo)[SIFT_BOUND_BYTES])
{
	SiftMaintSwitch sw = {.part_first = part_first,.part_n = part_n,.part_lo = part_lo,.old = old,.nold = nold,.neu = nneu > 0 ? neu : NULL,.more = nneu > 1 ? neu + 1 : NULL,.nmore = Max(nneu - 1, 0),.inserted = inserted};
	const SiftMaintOps *ops = sift_mops_of(index);
	uint64		bytes = 0;

	for (int i = 0; i < nneu; i++)
		bytes += neu[i].bytes;
	maint_written += bytes;
	sift_progress_step(nold, bytes);

	if (ops && ops->switching)
		ops->switching(index, old, nold);
	sift_dir_publish(index, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,.release = release,
			.nrelease = nrelease,.nadd = Max(nneu, 1),.extra = InvalidBuffer}, wt);
	if (ops && ops->switched)
		ops->switched(index);
	sift_log_maint(index, what, start, docs, bytes);
}

void
sift_maint_replace_n(Relation index, SiftWriteTarget *wt, const uint64 *old, int nold, const SiftSegEntry *neu, int nneu, SiftExtent *release, int nrelease,
					 const char *what, TimestampTz start, int64 docs)
{
	replace_n(index, wt, old, nold, neu, nneu, release, nrelease, what, start, docs, 0, 0, 0, NULL);
}

/* The same where neu[part_first, part_first + part_n) are partitions of the key, one each, in the order of the key: they take the lineage of the partitions they replace (sift_dir_part_replace) */
void
sift_maint_replace_partitions(Relation index, const uint64 *old, int nold, const SiftSegEntry *neu, int nneu, SiftExtent *release, int nrelease,
							  const char *what, TimestampTz start, int64 docs, int part_first, int part_n, const uint8 (*part_lo)[SIFT_BOUND_BYTES])
{
	replace_n(index, NULL, old, nold, neu, nneu, release, nrelease, what, start, docs, 0, part_first, part_n, part_lo);
}

void
sift_maint_promote_n(Relation index, uint64 sealed, uint32 records, const SiftSegEntry *neu, int nneu, SiftExtent *release, int nrelease, const char *what,
					 TimestampTz start, int64 docs)
{
	replace_n(index, NULL, &sealed, 1, neu, nneu, release, nrelease, what, start, docs, records, 0, 0, NULL);
}

void
sift_maint_replace(Relation index, SiftWriteTarget *wt, const uint64 *old, int nold, const SiftSegEntry *neu, SiftExtent *release, int nrelease,
				   const char *what, TimestampTz start, int64 docs)
{
	sift_maint_replace_n(index, wt, old, nold, neu, neu ? 1 : 0, release, nrelease, what, start, docs);
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
	SiftDirSnapshot *snap;
	bool		room;

	for (;;)
	{
		SiftStepResult	r;
		uint64		busy = 0;

		snap = sift_dir_read(index);
		if (snap->nsegs < room_segments(snap))
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
	room = snap->nsegs < room_segments(snap);
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
	SiftDirSnapshot *snap;
	SiftErrCtx	ctx;
	bool		room = false;

	snap = sift_dir_read(index);
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
	SiftDirSnapshot *snap;
	int			nsealed = 0;

	snap = sift_dir_read(index);
	if (snap->nsegs >= room_segments(snap))
		sift_mops_make_room(ops, index);
	if (ops->seal(index, false) == 0)
	{
		pfree(snap);
		return;
	}
	if (sift_async_maintenance)
		sift_maint_request_worker(index, ops->worker);
	pfree(snap);
	snap = sift_dir_read(index);
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
	sift_refuse_partitioned_index(index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX, RelationGetRelationName(index));
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

/* Seals the active segment and promotes every sealed one; returns the number promoted */
void
sift_mops_cut_free_end(const SiftMaintOps *ops, Relation index)
{
	cut_free_end(ops, index, false);
}

int
sift_mops_rewrite(const SiftMaintOps *ops, Relation index)
{
	return sift_mops_rewrite_ex(ops, index, NULL);
}

/* A rewrite pass; promoted, when given, is what the pass promoted from the write buffer */
int
sift_mops_rewrite_ex(const SiftMaintOps *ops, Relation index, int *promoted)
{
	const char *why;
	TimestampTz start = GetCurrentTimestamp();
	SiftErrCtx	ctx;
	bool		all,
				began;
	int			n = 0,
				sealed;

	if (!maint_lock_mode(index, false, ops->step_lock))
		return -1;
	sift_errctx_push(&ctx, RelationGetRelid(index));
	if ((why = ops->rewrite_due(index)) == NULL)
	{
		sift_errctx_pop(&ctx);
		maint_unlock_mode(index, ops->step_lock);
		return -1;
	}
	all = ops->rewrite_all == NULL || ops->rewrite_all(why);
	began = sift_progress_begin(index, SIFT_JOB_REWRITE);
	sift_progress_phase(index, "promote");
	sealed = promote_locked(ops, index, true, true);
	if (promoted != NULL)
		*promoted = sealed;
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
		if (n >= SIFT_DIR_POLICY_MAX)
			break;
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

static void
stamp_request(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->maint_requested_at = *(TimestampTz *) arg;
}

/* By oid: what a commit callback has, where no relation can be opened */
bool
sift_maint_register_worker_oid(Oid indexoid, SiftWorkerKind kind, const char *subject)
{
	return sift_worker_register(kind, subject, ObjectIdGetDatum(indexoid));
}

bool
sift_maint_register_worker(Relation index, SiftWorkerKind kind)
{
	return sift_maint_register_worker_oid(RelationGetRelid(index), kind, sift_worker_subject(index));
}

/*
 * A worker that stays (sift_maint_worker_stay) holds this session lock of its index from before its first look until it decided to leave. A request that finds it held is
 * served by that worker's last look (made after the lock is let go, so a request that saw it held is in the index by then); one that finds it free starts a worker.
 */
static void
stay_tag(LOCKTAG *tag, Oid indexoid)
{
	SET_LOCKTAG_ADVISORY(*tag, MyDatabaseId, 0x51F9, indexoid, 3);
}

static bool
stay_alive(Oid indexoid)
{
	LOCKTAG		tag;

	stay_tag(&tag, indexoid);
	if (LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
		return true;
	LockRelease(&tag, ExclusiveLock, true);
	return false;
}

bool
sift_maint_stay_begin(Oid indexoid)
{
	LOCKTAG		tag;

	stay_tag(&tag, indexoid);
	return LockAcquire(&tag, ExclusiveLock, true, true) != LOCKACQUIRE_NOT_AVAIL;
}

static void
stay_end(Oid indexoid)
{
	LOCKTAG		tag;

	stay_tag(&tag, indexoid);
	LockRelease(&tag, ExclusiveLock, true);
}

bool
sift_maint_request_worker(Relation index, SiftWorkerKind kind)
{
	SiftDirSnapshot *snap;
	TimestampTz now = GetCurrentTimestamp();
	bool		stays = sift_worker_stays(kind);
	bool		started;

	if (stays && stay_alive(RelationGetRelid(index)))
		return false;
	snap = sift_dir_read(index);
	started = (stays ? !in_backoff(&snap->meta) : now - snap->meta.maint_requested_at >= backoff_ms(&snap->meta) * INT64CONST(1000)) && !sift_index_out(index, snap->meta.flags);
	pfree(snap);
	if (!started)
		return false;
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_request,.arg = &now,.extra = InvalidBuffer});
	return sift_maint_register_worker(index, kind);
}

bool
sift_maint_busy(int max_active)
{
	return max_active > 0 && sift_pacer_running_backends(MyDatabaseId) > max_active;
}

Oid
sift_maint_worker_connect(Datum main_arg, ErrorContextCallback *errctx)
{
	Oid			indexoid = DatumGetObjectId(main_arg);

	sift_maint_context(errctx, indexoid);
	return indexoid;
}

/*
 * The worker looks for work every so often while it is up and leaves after two looks that find nothing, or when sift.maintenance_naptime after it started has passed
 * if the lock was held at its looks (an inserter promotes and leaves the merges to the worker, which would be left undone). A request finds the worker alive by the lock
 * it holds (stay_alive) and asks for no worker of its own; the worker lets the lock go before its last look, so a request that found it held is in the index by then,
 * and one that finds it free starts another.
 */
void
sift_maint_worker_stay(Oid indexoid, TimestampTz start, SiftWorkerStep step)
{
	long		look = 100;
	int			idle = 0;
	bool		yield = false;
	SiftStepResult r;

	for (;;)
	{
		long		left = sift_maintenance_naptime - TimestampDifferenceMilliseconds(start, GetCurrentTimestamp());
		bool		worked = false;

		while (!yield && (r = step(indexoid, start, false)) == SIFT_STEP_DONE)
		{
			worked = true;
			CHECK_FOR_INTERRUPTS();
			yield = sift_worker_should_yield();
		}
		idle = worked || r == SIFT_STEP_BUSY ? 0 : idle + 1;
		if (r == SIFT_STEP_FAILED || left <= 0 || idle >= 2 || yield)
			break;
		sift_worker_nap(Min(left, look));
		look = Min(2 * look, 1000);
	}
#ifdef SIFT_TESTING
	if (sift_test_stay_gate)
	{
		StartTransactionCommand();
		sift_test_wait(12);
		CommitTransactionCommand();
	}
#endif
	stay_end(indexoid);
	while (r != SIFT_STEP_FAILED && !yield && (r = step(indexoid, start, true)) == SIFT_STEP_DONE)
	{
		CHECK_FOR_INTERRUPTS();
		yield = sift_worker_should_yield();
	}
}

