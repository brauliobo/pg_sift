/*
 * core/worker.h - the background workers of sift: what each one is called (bgw_name "sift <job> <subject>", bgw_type "sift <job>", and the query of its
 * pg_stat_activity row "sift <job> of <subject>"), the note of the last step that ended well (sift.workers), and the registration of a worker
 */
#ifndef SIFT_WORKER_H
#define SIFT_WORKER_H

#include "datatype/timestamp.h"
#include "utils/rel.h"

/* The job of a worker, stored in the metapage as the job of the last step that ended well: 0 is none */
typedef enum SiftJob
{
	SIFT_JOB_NONE,
	SIFT_JOB_PROMOTE,
	SIFT_JOB_MERGE,
	SIFT_JOB_REWRITE,
	SIFT_JOB_VACUUM,
	SIFT_JOB_TRACK,
	SIFT_JOB_ROLLUP,
	SIFT_JOB_ORPHANS,
	SIFT_JOB_TABLE,
	SIFT_JOB_QUARANTINE,
	SIFT_NJOBS
} SiftJob;

/* The name of a job ("rewrite"), NULL for none or a number that is no job */
extern const char *sift_job_name(int job);

/* The subject of a worker for an index, "schema.index", and of one for the database of the caller, "database name" (palloc'd) */
extern char *sift_worker_subject(Relation index);
extern char *sift_worker_subject_db(void);

/*
 * Registers the dynamic worker that runs function (of pg_sift) for subject: its job is the one the function serves. main_arg is the argument the function gets;
 * the database of the caller is the one the worker connects to. False when fewer than max(2, max_worker_processes / 4) slots would stay free for parallel
 * queries and the workers of others, or the server has no slot: one LOG line a minute for every job says so (sift: no background worker for ...).
 */
extern bool sift_worker_register(const char *function, const char *subject, Datum main_arg);

/* Does the worker of this entry point look for work again while it is up (the promotion and merge workers)? */
extern bool sift_worker_stays(const char *function);

/* The first thing a worker does once it is connected: its job and subject are in pg_stat_activity from then on */
extern void sift_worker_announce(void);

/* The job of this process when it is a worker of sift, else SIFT_JOB_NONE; and whether the writes of its steps are paced (every job but the rollup refresh) */
extern SiftJob sift_worker_job(void);
extern bool sift_worker_paced(void);

/* The subject of this worker, as its name has it ("schema.index"; "database name" for a job of the database) */
extern const char *sift_worker_target(void);

/* A nap of a worker: idle in pg_stat_activity for its length */
extern void sift_worker_nap(long ms);

/* The metapage notes that the step of job that began at start ended well (sift.workers last_ok, last_job, last_ms) */
extern void sift_worker_ok(Relation index, SiftJob job, TimestampTz start);

/* sift.test_worker_error = 'vacuum': the worker of that job fails with an error where it reaches this */
#ifdef SIFT_TESTING
extern PGDLLIMPORT char *sift_test_worker_error;
extern void sift_test_worker_fail(SiftJob job);
#define SIFT_TEST_WORKER_FAIL(job)	sift_test_worker_fail(job)
#else
#define SIFT_TEST_WORKER_FAIL(job)	((void) 0)
#endif

#endif							/* SIFT_WORKER_H */
