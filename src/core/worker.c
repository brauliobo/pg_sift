/*
 * core/worker.c - see worker.h
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "commands/dbcommands.h"
#include "port/atomics.h"
#include "utils/backend_status.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "col/dsmreg.h"
#include "core/sift.h"
#include "core/store.h"
#include "core/worker.h"

/* The job each entry point of a worker serves */
static const struct
{
	const char *function;
	SiftJob		job;
}			job_of_function[] = {
	{"sift_maint_worker_main", SIFT_JOB_MERGE},
	{"sift_read_promote_worker_main", SIFT_JOB_PROMOTE},
	{"sift_col_maint_worker_main", SIFT_JOB_MERGE},
	{"sift_col_read_promote_worker_main", SIFT_JOB_PROMOTE},
	{"sift_col_rewrite_worker_main", SIFT_JOB_REWRITE},
	{"sift_col_read_rewrite_worker_main", SIFT_JOB_REWRITE},
	{"sift_col_decide_worker_main", SIFT_JOB_REWRITE},
	{"sift_col_vacuum_worker_main", SIFT_JOB_VACUUM},
	{"sift_col_vacuum_scan_worker_main", SIFT_JOB_VACUUM},
	{"sift_col_vacuum_once_worker_main", SIFT_JOB_VACUUM},
	{"sift_col_track_worker_main", SIFT_JOB_TRACK},
	{"sift_col_rollup_worker_main", SIFT_JOB_ROLLUP},
	{"sift_col_track_orphans_main", SIFT_JOB_ORPHANS},
	{"sift_tbl_maint_worker_main", SIFT_JOB_TABLE},
	{"sift_tbl_settle_worker_main", SIFT_JOB_TABLE},
	{"sift_quarantine_main", SIFT_JOB_QUARANTINE},
};

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
	return psprintf("%s.%s", get_namespace_name(RelationGetNamespace(index)), RelationGetRelationName(index));
}

char *
sift_worker_subject_db(void)
{
	return psprintf("database %s", get_database_name(MyDatabaseId));
}

static SiftJob
job_of(const char *function)
{
	for (int i = 0; i < lengthof(job_of_function); i++)
		if (strcmp(job_of_function[i].function, function) == 0)
			return job_of_function[i].job;
	SIFT_INVARIANT(false, "sift: no job for the worker function \"%s\"", function);
	return SIFT_JOB_NONE;
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
	[SIFT_JOB_REWRITE] = "The next VACUUM, and the next read or write that finds the rewrite due, ask for it again.",
	[SIFT_JOB_VACUUM] = "The next read or write that finds the table changed asks for the VACUUM again, and autovacuum vacuums it.",
	[SIFT_JOB_TRACK] = "The next read of the log asks for it again.",
	[SIFT_JOB_ROLLUP] = "The requests stay queued; sift.col_rollup_refresh(index) refreshes them in a session.",
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
	pg_atomic_init_u64(ptr, 0);
}

/* The time of the last line, kept in shared memory: the sessions that ask for a worker are short, and one line a minute is for the server */
static bool
refused_due(void)
{
	static pg_atomic_uint64 own;
	static pg_atomic_uint64 *last;
	uint64		seen;
	uint64		now = GetCurrentTimestamp();

	if (last == NULL && (last = sift_dsm_try("sift_refused_1", sizeof(*last), refused_init)) == NULL)
		last = &own;
	seen = pg_atomic_read_u64(last);
	return now - seen >= USECS_PER_MINUTE && pg_atomic_compare_exchange_u64(last, &seen, now);
}

/* One line a minute, for every kind of worker: the server has no slot to give it */
static void
refused(SiftJob job, const char *subject, int ours, int reserve)
{
	if (!refused_due())
		return;
	ereport(LOG,
			(for_index(job) ? errmsg("sift: no background worker for %s of index \"%s\"", job_names[job], subject)
			 : errmsg("sift: no background worker for %s of %s", job_names[job], subject),
			 errdetail("max_worker_processes is %d; sift workers hold %d of the slots, and sift leaves %d of them for parallel queries and other workers.",
					   max_worker_processes, ours, reserve),
			 errhint("%s Raise max_worker_processes to run more at a time.", job_hints[job])));
}

/* The slots that background workers hold, and how many of them are sift's: a worker with a database connection has a row in the activity of the server */
static int
slots_held(int *ours)
{
	int			all = 0;

	*ours = 0;
	pgstat_clear_backend_activity_snapshot();
	for (int i = 1, max = pgstat_fetch_stat_numbackends(); i <= max; i++)
	{
		const PgBackendStatus *s = &pgstat_get_local_beentry_by_index(i)->backendStatus;
		const char *type;

		if (s->st_backendType != B_BG_WORKER)
			continue;
		all++;
		type = GetBackgroundWorkerTypeByPid(s->st_procpid);
		*ours += type != NULL && strncmp(type, "sift ", 5) == 0;
	}
	return all;
}

bool
sift_worker_register(const char *function, const char *subject, Datum main_arg)
{
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	SiftJob		job = job_of(function);
	int			ours;
	int			reserve = Max(2, max_worker_processes / 4);

	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION | SIFT_BGW_INTERRUPTIBLE;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "%s", function);
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift %s %s", job_names[job], subject);
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift %s", job_names[job]);
	bw.bgw_main_arg = main_arg;
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	/* the last slots are for parallel queries and the workers of others: sift leaves reserve of them free */
	if (max_worker_processes - slots_held(&ours) <= reserve || !RegisterDynamicBackgroundWorker(&bw, &handle))
	{
		refused(job, subject, ours, reserve);
		return false;
	}
	return true;
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

bool
sift_worker_paced(void)
{
	SiftJob		job = sift_worker_job();

	return job != SIFT_JOB_NONE && job != SIFT_JOB_ROLLUP;
}

const char *
sift_worker_target(void)
{
	return MyBgworkerEntry->bgw_name + strlen(MyBgworkerEntry->bgw_type) + 1;
}

void
sift_worker_announce(void)
{
	pgstat_report_activity(STATE_RUNNING, psprintf("%s of %s", MyBgworkerEntry->bgw_type, sift_worker_target()));
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

#ifdef SIFT_TESTING
char	   *sift_test_worker_error = NULL;

void
sift_test_worker_fail(SiftJob job)
{
	if (sift_test_worker_error != NULL && strcmp(sift_test_worker_error, job_names[job]) == 0)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift test: the %s worker fails", job_names[job]), errhint("sift.test_worker_error is set.")));
}
#endif
