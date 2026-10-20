/*
 * col/rollup.c - the on-demand refresh of the rollups of the units (docs/COL-PREAGG.md section 7).
 *
 * A SELECT that has to scan an exact unit because its rollup is missing, was built with other dims or lacks a measure the query reads records the
 * segment of the unit in a small queue in shared memory (a named DSM segment: no shared_preload_libraries needed) and starts a background worker
 * for the database. The SELECT writes no page and its result does not change. The worker rewrites the queued segments one at a time, as a merge
 * of that segment alone does, with the pre-aggregates the index has now: the units of the segment get their rollup, and the next SELECT answers
 * from them. The rewrite is the atomic replacement of the maintenance (docs/COL.md 2.6), under the maintenance lock, so that it is safe with
 * VACUUM, merges and concurrent DML; a segment that is no longer in the directory is skipped. The refresh is by segment because the units of a
 * segment are one file; it rewrites the units of the segment that already had their rollup too.
 *
 * A refreshed segment is remembered (index, generation, options) so that a unit whose rollup cannot be built (more tuples than the cap) is
 * not requested again and again; the memory is lost with a restart of the server, which costs one more rewrite per segment.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pagg.h"
#include "col/seg.h"
#include "bm25/maint.h"
#include "core/ops.h"

#define ROLLUP_QUEUE		64		/* segments waiting */
#define ROLLUP_DONE			256		/* refreshed segments remembered */
#define ROLLUP_WORKERS		8		/* databases with a worker running */
#define ROLLUP_LAUNCH_US	(2 * USECS_PER_SEC)		/* the least time between two launches for a database */

bool		sift_col_rollup_on_demand = true;
bool		sift_col_rollup_worker = true;
#ifdef SIFT_TESTING
int			sift_test_col_rollup_panic = 0;
#endif

typedef struct RollupReq
{
	Oid			db;
	Oid			index;
	uint64		gen;
	uint32		hash;			/* of the options of the index that decide what a rollup holds */
	uint32		units;
	bool		busy;			/* a worker is at it */
	TimestampTz at;
} RollupReq;

typedef struct RollupDone
{
	Oid			index;
	uint64		gen;
	uint32		hash;
} RollupDone;

typedef struct RollupShared
{
	slock_t		lock;
	int			nreq;
	int			ndone;
	int			donepos;
	int			nworkers;
	Oid			worker_db[ROLLUP_WORKERS];
	TimestampTz last_launch[ROLLUP_WORKERS];
	int64		requested;		/* units */
	int64		refreshed;
	int64		skipped;
	int64		dropped;
	RollupReq	req[ROLLUP_QUEUE];
	RollupDone	done[ROLLUP_DONE];
} RollupShared;

static RollupShared *rollup_shared;

/* PostgreSQL 19 passes the argument of GetNamedDSMSegment to the init callback */
#if PG_VERSION_NUM >= 190000
static void
rollup_init(void *ptr, void *arg)
#else
static void
rollup_init(void *ptr)
#endif
{
	RollupShared *sh = ptr;

	memset(sh, 0, sizeof(*sh));
	SpinLockInit(&sh->lock);
}

static RollupShared *
shared(void)
{
	bool		found;

	if (rollup_shared == NULL)
#if PG_VERSION_NUM >= 190000
		rollup_shared = GetNamedDSMSegment("pg_sift_col_rollup", sizeof(RollupShared), rollup_init, &found, NULL);
#else
		rollup_shared = GetNamedDSMSegment("pg_sift_col_rollup", sizeof(RollupShared), rollup_init, &found);
#endif
	return rollup_shared;
}

/* what decides the rollup an index writes: its options */
static uint32
options_hash(Relation index)
{
	const char *a = sift_col_options_preagg(index),
			   *b = sift_col_options_rollup(index);

	return hash_bytes((const unsigned char *) a, strlen(a)) ^ (hash_bytes((const unsigned char *) b, strlen(b)) * 31);
}

static bool
is_done(const RollupShared *sh, Oid index, uint64 gen, uint32 hash)
{
	for (int i = 0; i < sh->ndone; i++)
		if (sh->done[i].index == index && sh->done[i].gen == gen && sh->done[i].hash == hash)
			return true;
	return false;
}

/* ---------- the worker ---------- */

PGDLLEXPORT void sift_col_rollup_worker_main(Datum main_arg);

static void
release_worker(Oid db)
{
	RollupShared *sh = shared();

	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nworkers; i++)
		if (sh->worker_db[i] == db)
		{
			sh->worker_db[i] = sh->worker_db[--sh->nworkers];
			sh->last_launch[i] = sh->last_launch[sh->nworkers];
			break;
		}
	SpinLockRelease(&sh->lock);
}

static void
launch_worker(void)
{
	RollupShared *sh = shared();
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	TimestampTz now = GetCurrentTimestamp();
	int			slot = -1;

	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nworkers; i++)
		if (sh->worker_db[i] == MyDatabaseId)
			slot = i;
	if (slot < 0 && sh->nworkers < ROLLUP_WORKERS)
	{
		slot = sh->nworkers++;
		sh->worker_db[slot] = MyDatabaseId;
		sh->last_launch[slot] = now;
	}
	else
		slot = -1;
	SpinLockRelease(&sh->lock);
	if (slot < 0)
		return;
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_col_rollup_worker_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift rollup refresh (database %u)", MyDatabaseId);
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift rollup refresh");
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	if (!RegisterDynamicBackgroundWorker(&bw, &handle))
		release_worker(MyDatabaseId);
}

/* a request of the database (of the index, when given) not taken yet, marked busy; false when there is none */
static bool
pop(Oid db, Oid index, RollupReq *out)
{
	RollupShared *sh = shared();
	bool		found = false;

	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nreq && !found; i++)
		if (sh->req[i].db == db && (index == InvalidOid || sh->req[i].index == index) && !sh->req[i].busy)
		{
			sh->req[i].busy = true;
			*out = sh->req[i];
			found = true;
		}
	SpinLockRelease(&sh->lock);
	return found;
}

/* the request leaves the queue: its units were refreshed (done) or skipped */
static void
finish(const RollupReq *r, bool done, uint64 newgen)
{
	RollupShared *sh = shared();

	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nreq; i++)
		if (sh->req[i].db == r->db && sh->req[i].index == r->index && sh->req[i].gen == r->gen)
		{
			if (done)
				sh->refreshed += sh->req[i].units;
			else
				sh->skipped += sh->req[i].units;
			sh->req[i] = sh->req[--sh->nreq];
			break;
		}
	if (done && newgen != 0)
	{
		sh->done[sh->donepos] = (RollupDone) {r->index, newgen, r->hash};
		sh->donepos = (sh->donepos + 1) % ROLLUP_DONE;
		sh->ndone = Min(sh->ndone + 1, ROLLUP_DONE);
	}
	SpinLockRelease(&sh->lock);
}

static void
worker_exit(int code, Datum arg)
{
	RollupShared *sh = shared();
	Oid			db = DatumGetObjectId(arg);

	/* the requests this worker had taken die with it */
	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nreq;)
		if (sh->req[i].db == db && sh->req[i].busy)
		{
			sh->skipped += sh->req[i].units;
			sh->req[i] = sh->req[--sh->nreq];
		}
		else
			i++;
	SpinLockRelease(&sh->lock);
	release_worker(db);
}

/* the generations of the immutable segments of the directory that are not in before */
static uint64
new_gen(Relation index, const SiftDirSnapshot *before)
{
	SiftDirSnapshot *after = palloc(sizeof(SiftDirSnapshot));
	uint64		gen = 0;

	sift_dir_read(index, after);
	for (int i = 0; i < after->nsegs; i++)
	{
		bool		old = false;

		for (int j = 0; j < before->nsegs && !old; j++)
			old = before->segs[j].gen == after->segs[i].gen;
		if (!old && after->segs[i].kind == SIFT_SEG_IMMUTABLE)
			gen = after->segs[i].gen;
	}
	pfree(after);
	return gen;
}

/* The segment of the request is rewritten alone with the options the index has now, under the maintenance lock. Returns the generation of the result, 0 when skipped */
static uint64
refresh_segment(Relation index, const RollupReq *r)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftColRel *rel = sift_col_rel(index);
	uint64		gen = 0;

	sift_maint_lock(index, true);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == r->gen && snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftSegEntry e = snap->segs[i];

#ifdef SIFT_TESTING
			sift_test_col_rollup_panic_armed = sift_test_col_rollup_panic;
#endif
			sift_col_merge_segments(index, rel->schema, rel->policy, &rel->pagg, &e, 1, sift_col_format_minor, NULL, NULL);
			gen = new_gen(index, snap);
			break;
		}
	sift_maint_unlock(index);
	pfree(snap);
	return gen;
}

static void
process(const RollupReq *r)
{
	Relation	index;
	uint64		gen = 0;
	bool		ok = false;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	index = try_index_open(r->index, RowExclusiveLock);
	if (index != NULL && sift_col_is_index(index) && !RecoveryInProgress() && options_hash(index) == r->hash && sift_col_format_minor >= 4)
	{
		gen = refresh_segment(index, r);
		ok = gen != 0;
	}
	if (index != NULL)
		index_close(index, RowExclusiveLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
	finish(r, ok, gen);
}

void
sift_col_rollup_worker_main(Datum main_arg)
{
	Oid			db;
	RollupReq	r;

	memcpy(&db, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(db, InvalidOid, 0);
	before_shmem_exit(worker_exit, ObjectIdGetDatum(db));
	while (pop(db, InvalidOid, &r))
		process(&r);
}

/* ---------- the request, from a SELECT ---------- */

PG_FUNCTION_INFO_V1(sift_col_rollup_refresh);

/* sift.col_rollup_refresh(index): the queued requests of the index are refreshed now, in this session; the number of segments rewritten */
Datum
sift_col_rollup_refresh(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), RowExclusiveLock);
	RollupReq	r;
	int			n = 0;

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	sift_check_maintain(index);
	if (RecoveryInProgress())
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("recovery is in progress"),
						errhint("The rollups are refreshed on the primary.")));
	while (pop(MyDatabaseId, RelationGetRelid(index), &r))
	{
		uint64		gen = options_hash(index) == r.hash ? refresh_segment(index, &r) : 0;

		finish(&r, gen != 0, gen);
		n += gen != 0;
	}
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

bool
sift_col_rollup_request(Relation index, uint64 gen)
{
	RollupShared *sh;
	uint32		hash;
	TimestampTz now = GetCurrentTimestamp();
	bool		queued = false;

	/* a server that writes minor 3 (sift.col_format_minor, for readers of 3.0) is not rewritten to minor 4 behind its back */
	if (!sift_col_rollup_on_demand || !sift_col_options(index)->rollup_on_demand || RecoveryInProgress() || sift_col_format_minor < 4)
		return false;
	sh = shared();
	hash = options_hash(index);
	SpinLockAcquire(&sh->lock);
	if (is_done(sh, RelationGetRelid(index), gen, hash))
		sh->skipped++;
	else
	{
		int			i = 0;

		while (i < sh->nreq && !(sh->req[i].db == MyDatabaseId && sh->req[i].index == RelationGetRelid(index) && sh->req[i].gen == gen))
			i++;
		sh->requested++;
		if (i < sh->nreq)
			sh->req[i].units++;
		else if (sh->nreq < ROLLUP_QUEUE)
			sh->req[sh->nreq++] = (RollupReq) {MyDatabaseId, RelationGetRelid(index), gen, hash, 1, false, now};
		else
			sh->dropped++;
		queued = i < sh->nreq;
	}
	SpinLockRelease(&sh->lock);
	if (queued && sift_col_rollup_worker)
		launch_worker();
	return queued;
}

/* ---------- SQL: sift.col_rollup_counters, sift.col_rollup_queue ---------- */

PG_FUNCTION_INFO_V1(sift_col_rollup_counters);
PG_FUNCTION_INFO_V1(sift_col_rollup_queue);

Datum
sift_col_rollup_counters(PG_FUNCTION_ARGS)
{
	RollupShared *sh = shared();
	TupleDesc	tupdesc;
	Datum		v[6];
	bool		nulls[6] = {false};
	int			queued = 0,
				busy = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nreq; i++)
		if (sh->req[i].db == MyDatabaseId)
		{
			queued++;
			busy += sh->req[i].busy;
		}
	v[0] = Int64GetDatum(sh->requested);
	v[1] = Int64GetDatum(sh->refreshed);
	v[2] = Int64GetDatum(sh->skipped);
	v[3] = Int64GetDatum(sh->dropped);
	v[4] = Int32GetDatum(queued - busy);
	v[5] = Int32GetDatum(busy);
	SpinLockRelease(&sh->lock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

Datum
sift_col_rollup_queue(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	RollupShared *sh = shared();
	RollupReq  *copy = palloc(ROLLUP_QUEUE * sizeof(RollupReq));
	int			n = 0;

	InitMaterializedSRF(fcinfo, 0);
	SpinLockAcquire(&sh->lock);
	for (int i = 0; i < sh->nreq; i++)
		if (sh->req[i].db == MyDatabaseId)
			copy[n++] = sh->req[i];
	SpinLockRelease(&sh->lock);
	for (int i = 0; i < n; i++)
	{
		Datum		v[5] = {ObjectIdGetDatum(copy[i].index), Int64GetDatum(copy[i].gen), Int32GetDatum(copy[i].units),
		TimestampTzGetDatum(copy[i].at), CStringGetTextDatum(copy[i].busy ? "refreshing" : "queued")};
		bool		nulls[5] = {false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}
