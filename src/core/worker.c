/*
 * core/worker.c - see worker.h
 */
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "commands/dbcommands.h"
#include "lib/stringinfo.h"
#include "port/atomics.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "col/dsmreg.h"
#include "core/errctx.h"
#include "core/maint_pacer.h"
#include "core/maint_sched.h"
#include "core/sift.h"
#include "core/store.h"
#include "core/worker.h"
#include "utils/backend_status.h"
#include "pgstat.h"

/* The job each kind of worker serves, and the tier of its steps (core/maint_pacer.h) */
static const struct
{
	SiftJob		job;
	SiftPacerTier tier;
	bool		stays;			/* looks for work again while it is up (sift_maint_worker_stay): its index counts it as alive */
}			kinds[SIFT_WK_N] = {
	[SIFT_WK_BM25_MAINT] = {SIFT_JOB_MERGE, SIFT_TIER_P1, true},
	[SIFT_WK_BM25_READ_PROMOTE] = {SIFT_JOB_PROMOTE, SIFT_TIER_P0, true},
	[SIFT_WK_COL_MAINT] = {SIFT_JOB_MERGE, SIFT_TIER_P1, true},
	[SIFT_WK_COL_READ_PROMOTE] = {SIFT_JOB_PROMOTE, SIFT_TIER_P0, true},
	[SIFT_WK_COL_TRACK] = {SIFT_JOB_TRACK, SIFT_TIER_P1},
	[SIFT_WK_COL_ROLLUP] = {SIFT_JOB_ROLLUP, SIFT_TIER_P1},
	[SIFT_WK_COL_ORPHANS] = {SIFT_JOB_ORPHANS, SIFT_TIER_P3},
	[SIFT_WK_TBL_MAINT] = {SIFT_JOB_TABLE, SIFT_TIER_P2},
	[SIFT_WK_TBL_SETTLE] = {SIFT_JOB_TABLE, SIFT_TIER_P0},
	[SIFT_WK_QUARANTINE] = {SIFT_JOB_QUARANTINE, SIFT_TIER_P0},
};

static SiftWorkerMain mains[SIFT_WK_N];

void
sift_worker_define(SiftWorkerKind kind, SiftWorkerMain main)
{
	mains[kind] = main;
}

static const char *const job_names[SIFT_NJOBS] = {
	[SIFT_JOB_PROMOTE] = "promote",
	[SIFT_JOB_MERGE] = "merge",
	[SIFT_JOB_REWRITE] = "rewrite",
	[SIFT_JOB_VACUUM] = "vacuum",
	[SIFT_JOB_TRACK] = "track",
	[SIFT_JOB_ROLLUP] = "rollup",
	[SIFT_JOB_ORPHANS] = "orphans",
	[SIFT_JOB_TABLE] = "table",
	[SIFT_JOB_QUARANTINE] = "quarantine",
};

const char *
sift_job_name(int job)
{
	return job > SIFT_JOB_NONE && job < SIFT_NJOBS ? job_names[job] : NULL;
}

char *
sift_worker_subject(Relation index)
{
	const char *frozen = sift_errctx_frozen_name(RelationGetRelid(index));

	if (frozen != NULL)
		return pstrdup(frozen);
	return psprintf("%s.%s", get_namespace_name(RelationGetNamespace(index)), RelationGetRelationName(index));
}

char *
sift_worker_subject_db(void)
{
	return psprintf("database %s", get_database_name(MyDatabaseId));
}

bool
sift_worker_stays(SiftWorkerKind kind)
{
	return kinds[kind].stays;
}

/* A flag of the server (19 and later) that ends a worker whose database is created from, altered or dropped, so that DROP DATABASE does not wait for it */
#ifdef BGWORKER_INTERRUPTIBLE
#define SIFT_BGW_INTERRUPTIBLE	BGWORKER_INTERRUPTIBLE
#else
#define SIFT_BGW_INTERRUPTIBLE	0
#endif

/* What a worker of each job tells the operator when it could not be started: what happens to the work instead */
static const char *const job_hints[SIFT_NJOBS] = {
	[SIFT_JOB_PROMOTE] = "The inserting sessions promote and merge instead.",
	[SIFT_JOB_MERGE] = "The inserting sessions promote and merge instead, and VACUUM merges.",
	[SIFT_JOB_REWRITE] = "Run SELECT * FROM sift.maintain(index), or REINDEX INDEX CONCURRENTLY.",
	[SIFT_JOB_TRACK] = "The next read of the log asks for it again.",
	[SIFT_JOB_ROLLUP] = "The requests stay queued; SELECT * FROM sift.maintain(index) refreshes them now.",
	[SIFT_JOB_ORPHANS] = "The triggers do nothing; the next statement of their table that fires one asks again.",
	[SIFT_JOB_TABLE] = "The next write asks for it again, and VACUUM does the same work.",
	[SIFT_JOB_QUARANTINE] = "The index stays out of service in the registry of the server only (persisted = false in sift.quarantine); the next plan that leaves it out asks again, at most every 10 seconds, and a restart before the flag is written forgets it until the damage is read again.",
};

/* Rollup and orphans work for the database, the others for an index */
static bool
for_index(SiftJob job)
{
	return job != SIFT_JOB_ROLLUP && job != SIFT_JOB_ORPHANS;
}

static void
refused_init(SIFT_DSM_INIT_ARGS)
{
	for (int j = 0; j < SIFT_NJOBS; j++)
		pg_atomic_init_u64(&((pg_atomic_uint64 *) ptr)[j], 0);
}

/*
 * The time of the last LOG line of each job, kept in shared memory: the sessions that ask for a worker are short, and a job that is refused again and again (the request comes with
 * every VACUUM, read or write that finds the work due) says so once an hour, and at DEBUG1 in between
 */
#define REFUSED_LOG_INTERVAL	(60 * USECS_PER_MINUTE)

static int
refused_level(SiftJob job)
{
	static pg_atomic_uint64 own[SIFT_NJOBS];
	static pg_atomic_uint64 *last;
	uint64		seen;
	uint64		now = GetCurrentTimestamp();

	/* MEM[MEM-SHM-12]: DSM worker refusal + slots sift_refused_2/sift_workers_2 (208 B fixed) */
	if (last == NULL && (last = sift_dsm_try("sift_refused_2", sizeof(own), refused_init)) == NULL)
		last = own;
	seen = pg_atomic_read_u64(&last[job]);
	return now - seen >= REFUSED_LOG_INTERVAL && pg_atomic_compare_exchange_u64(&last[job], &seen, now) ? LOG : DEBUG1;
}

/*
 * The workers of sift that are running or registered and not yet started, for all the databases of the server: one atomic word for each of the sift.max_maint_workers slots (the
 * first ones are the ones a request takes), no scan of the backends. A word is 0 for a free slot, else the time the slot was taken, shifted by one, with the low bit set once the worker is up.
 * The registrar takes the slot and the worker gives it back when it ends, whatever the way; a worker that never gets as far (the postmaster could not fork it, the library did not
 * load) would keep it for good, so a slot whose worker is not up SLOT_START_LEASE_US after it was taken is free again for the next request.
 */
#define WORKER_SLOTS		16
#define SLOT_STARTED		1
#define SLOT_START_LEASE_US	(30 * USECS_PER_SEC)

static int	sift_max_maint_workers = 4;

#ifdef SIFT_TESTING
int			sift_test_worker_lease_ms = 0;
#endif

typedef struct WorkerSlots
{
	pg_atomic_uint64 claim[WORKER_SLOTS];
} WorkerSlots;

/* The slot of this worker and the word its registrar took it with, and its kind */
static int	my_slot = -1;
static uint64 my_claim;
static SiftWorkerKind my_kind = SIFT_WK_N;

/* The tier of the claim a worker of the queue (core/maint_sched.h) runs; no other worker has one */
#define NOT_A_QUEUE_WORKER	(-1)
static int	queue_tier = NOT_A_QUEUE_WORKER;

static void
slots_init(SIFT_DSM_INIT_ARGS)
{
	WorkerSlots *w = ptr;

	for (int i = 0; i < WORKER_SLOTS; i++)
		pg_atomic_init_u64(&w->claim[i], 0);
}

static WorkerSlots *
worker_slots(void)
{
	static WorkerSlots own;
	static WorkerSlots *slots;

	if (slots == NULL && (slots = sift_dsm_try("sift_workers_2", sizeof(own), slots_init)) == NULL)
	{
		SIFT_DSM_INIT_CALL(slots_init, &own);
		slots = &own;
	}
	return slots;
}

static TimestampTz
start_lease(void)
{
#ifdef SIFT_TESTING
	if (sift_test_worker_lease_ms > 0)
		return (TimestampTz) sift_test_worker_lease_ms * 1000;
#endif
	return SLOT_START_LEASE_US;
}

static bool
slot_lapsed(uint64 word, TimestampTz now)
{
	return word != 0 && (word & SLOT_STARTED) == 0 && now - (TimestampTz) (word >> 1) > start_lease();
}

static int slots_taken(void);

static int
workers_cap(void)
{
	return sift_maint_workers_cap(sift_max_maint_workers, max_worker_processes);
}

int
sift_worker_legacy_active(void)
{
	return slots_taken();
}

static int
slot_claim(uint64 *claim)
{
	WorkerSlots *w = worker_slots();
	TimestampTz now = GetCurrentTimestamp();

	for (int i = 0; i < workers_cap(); i++)
	{
		uint64		seen = pg_atomic_read_u64(&w->claim[i]);

		if (slot_lapsed(seen, now) && pg_atomic_compare_exchange_u64(&w->claim[i], &seen, 0))
			seen = 0;
		*claim = (uint64) now << 1;
		if (seen == 0 && pg_atomic_compare_exchange_u64(&w->claim[i], &seen, *claim))
		{
			uint64		mine = *claim;

			if (slots_taken() + sift_maint_active_workers() <= workers_cap())
				return i;
			pg_atomic_compare_exchange_u64(&w->claim[i], &mine, 0);
			return -1;
		}
	}
	return -1;
}

/* Gives the slot back if it is still the one taken with claim (a slot that lapsed and was taken again is another worker's) */
static void
slot_give_back(int slot, uint64 claim)
{
	pg_atomic_uint64 *word = &worker_slots()->claim[slot];
	uint64		seen = pg_atomic_read_u64(word);

	if ((seen >> 1) == (claim >> 1))
		pg_atomic_compare_exchange_u64(word, &seen, 0);
}

static void
slot_release(int code, Datum arg)
{
	slot_give_back(my_slot, my_claim);
}

static void
slot_started(int slot, uint64 claim)
{
	uint64		expected = claim;

	pg_atomic_compare_exchange_u64(&worker_slots()->claim[slot], &expected, claim | SLOT_STARTED);
}

static int
slots_taken(void)
{
	int			n = 0;

	for (int i = 0; i < WORKER_SLOTS; i++)
		n += pg_atomic_read_u64(&worker_slots()->claim[i]) != 0;
	return n;
}

#ifdef SIFT_TESTING
static struct
{
	int			slot;
	uint64		claim;
}			held[WORKER_SLOTS];
static int	nheld;

static void
held_release(int code, Datum arg)
{
	sift_worker_test_hold(0);
}

int
sift_worker_test_hold(int n)
{
	static bool exit_hook;

	if (!exit_hook)
		before_shmem_exit(held_release, 0);
	exit_hook = true;
	while (nheld > 0)
	{
		nheld--;
		slot_give_back(held[nheld].slot, held[nheld].claim);
	}
	while (nheld < n && (held[nheld].slot = slot_claim(&held[nheld].claim)) >= 0)
		nheld++;
	return nheld;
}
#endif

/* One line for every kind of worker: sift.max_maint_workers are running, or the server has no slot */
static void
refused(SiftJob job, const char *subject)
{
	int			level = refused_level(job);

	if (!message_level_is_interesting(level))
		return;
	ereport(level,
			(for_index(job) ? errmsg("sift: no background worker for %s of index \"%s\"", job_names[job], subject)
			 : errmsg("sift: no background worker for %s of %s", job_names[job], subject),
			 errdetail("sift.max_maint_workers is %d and %d are running; max_worker_processes is %d.",
					   sift_max_maint_workers, slots_taken(), max_worker_processes),
			 errhint("%s Raise sift.max_maint_workers (and max_worker_processes) to run more at a time.", job_hints[job])));
}

/* What the workers of sift find in bgw_extra */
typedef struct WorkerExtra
{
	Oid			db;
	SiftWorkerKind kind;
	int			slot;
	uint64		claim;
} WorkerExtra;

StaticAssertDecl(sizeof(WorkerExtra) <= BGW_EXTRALEN, "bgw_extra holds the database and the kind of a worker");

bool
sift_worker_register(SiftWorkerKind kind, const char *subject, Datum main_arg)
{
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	SiftJob		job = kinds[kind].job;
	WorkerExtra extra = {.db = MyDatabaseId,.kind = kind};

	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION | SIFT_BGW_INTERRUPTIBLE;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_worker_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift %s %s", job_names[job], subject);
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift %s", job_names[job]);
	bw.bgw_main_arg = main_arg;
	if ((extra.slot = slot_claim(&extra.claim)) < 0)
	{
		sift_pacer_note_starved(kinds[kind].tier, GetCurrentTimestamp());
		refused(job, subject);
		return false;
	}
	memcpy(bw.bgw_extra, &extra, sizeof(extra));
	if (!RegisterDynamicBackgroundWorker(&bw, &handle))
	{
		slot_give_back(extra.slot, extra.claim);
		refused(job, subject);
		return false;
	}
	return true;
}

/*
 * The entry point of every worker: the slot it was registered with is given back when it ends, whatever the way, before the shared memory it counts in is detached (the postmaster restarts after a crash, which resets the count), then the
 * connection to the database of its registrar, the announcement in pg_stat_activity, and the main of its kind
 */
void
sift_worker_main(Datum main_arg)
{
	WorkerExtra extra;

	memcpy(&extra, MyBgworkerEntry->bgw_extra, sizeof(extra));
	my_slot = extra.slot;
	my_claim = extra.claim;
	my_kind = extra.kind;
	before_shmem_exit(slot_release, 0);
	slot_started(my_slot, my_claim);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(extra.db, InvalidOid, 0);
	if (sift_worker_paced())
		sift_maint_ring_open();
	pgstat_report_activity(STATE_RUNNING, psprintf("%s of %s", MyBgworkerEntry->bgw_type, sift_worker_target()));
	SIFT_INVARIANT(mains[extra.kind] != NULL, "sift: no main for the worker kind %d", (int) extra.kind);
	mains[extra.kind](main_arg);
}

void
sift_worker_init(void)
{
	DefineCustomIntVariable("sift.max_maint_workers",
							"The most background workers of sift (promotion, merge, tracking, rollup, table and quarantine workers) that run at once in the server.",
							"A request that finds that many running is refused with a LOG line at most once an hour for its job, and its work is left to the inserting sessions, VACUUM and the next request. "
							"The server's own max_worker_processes still bounds the sum.",
							&sift_max_maint_workers, 4, 0, WORKER_SLOTS, PGC_SIGHUP, 0, NULL, sift_maint_assign_max_workers, NULL);
}

/* The type of this worker is "sift <job>": the job is what follows, the subject what follows the type in its name */
SiftJob
sift_worker_job(void)
{
	if (MyBackendType != B_BG_WORKER || strncmp(MyBgworkerEntry->bgw_type, "sift ", 5) != 0)
		return SIFT_JOB_NONE;
	for (int j = SIFT_JOB_NONE + 1; j < SIFT_NJOBS; j++)
		if (strcmp(MyBgworkerEntry->bgw_type + 5, job_names[j]) == 0)
			return j;
	return SIFT_JOB_NONE;
}

void
sift_worker_queue_enter(void)
{
	queue_tier = SIFT_TIER_P0;
	sift_maint_ring_open();
}

void
sift_worker_queue_tier(SiftPacerTier tier)
{
	queue_tier = tier;
}

bool
sift_worker_in_queue(void)
{
	return queue_tier != NOT_A_QUEUE_WORKER;
}

bool
sift_worker_paced(void)
{
	SiftJob		job = sift_worker_job();

	return sift_worker_in_queue() || (job != SIFT_JOB_NONE && job != SIFT_JOB_ROLLUP);
}

int
sift_worker_tier(void)
{
	if (sift_worker_in_queue())
		return queue_tier;
	return my_kind == SIFT_WK_N ? SIFT_TIER_P0 : kinds[my_kind].tier;
}

bool
sift_worker_should_yield(void)
{
	if (sift_worker_in_queue())
		return sift_pacer_should_yield(queue_tier, GetCurrentTimestamp());
	return my_kind != SIFT_WK_N && sift_pacer_should_yield(kinds[my_kind].tier, GetCurrentTimestamp());
}

const char *
sift_worker_target(void)
{
	return MyBgworkerEntry->bgw_name + strlen(MyBgworkerEntry->bgw_type) + 1;
}

void
sift_worker_nap(long ms)
{
	pgstat_report_activity(STATE_IDLE, NULL);
	sift_nap(ms);
	pgstat_report_activity(STATE_RUNNING, NULL);
}

typedef struct OkNote
{
	SiftJob		job;
	TimestampTz at;
	uint32		ms;
} OkNote;

static bool
stamp_ok(SiftMetaPageData *meta, void *arg)
{
	const OkNote *n = arg;

	meta->maint_ok_at = n->at;
	meta->maint_ok_job = n->job;
	meta->maint_ok_ms = n->ms;
	return true;
}

void
sift_worker_ok(Relation index, SiftJob job, TimestampTz start)
{
	TimestampTz now = GetCurrentTimestamp();
	OkNote		n = {job, now, (uint32) Min((now - start) / 1000, PG_UINT32_MAX)};

	sift_meta_update(index, stamp_ok, &n);
}
