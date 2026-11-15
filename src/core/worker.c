/*
 * core/worker.c - see worker.h
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "commands/dbcommands.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

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

bool
sift_worker_register(const char *function, const char *subject, Datum main_arg)
{
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	const char *job = job_names[job_of(function)];

	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION | SIFT_BGW_INTERRUPTIBLE;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "%s", function);
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift %s %s", job, subject);
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift %s", job);
	bw.bgw_main_arg = main_arg;
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	return RegisterDynamicBackgroundWorker(&bw, &handle);
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
