/*
 * core/worker.h - the background workers of sift: what each one is called (bgw_name "sift <job> <subject>", bgw_type "sift <job>", and the query of its
 * pg_stat_activity row "sift <job> of <subject>"), the note of the last step that ended well (sift.workers), and the registration of a worker
 */
#ifndef SIFT_WORKER_H
#define SIFT_WORKER_H

#include "datatype/timestamp.h"
#include "utils/rel.h"

/* The entry points of the workers (pg_sift.so exports one: sift_worker_main), told apart by the kind in bgw_extra */
typedef enum SiftWorkerKind
{
	SIFT_WK_BM25_MAINT,
	SIFT_WK_BM25_READ_PROMOTE,
	SIFT_WK_COL_MAINT,
	SIFT_WK_COL_READ_PROMOTE,
	SIFT_WK_COL_TRACK,
	SIFT_WK_COL_ROLLUP,
	SIFT_WK_COL_ORPHANS,
	SIFT_WK_TBL_MAINT,
	SIFT_WK_TBL_SETTLE,
	SIFT_WK_QUARANTINE,
	SIFT_WK_N
} SiftWorkerKind;

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
 * What a kind of worker runs once it is connected to the database of its registrar (the signals, the connection and the announcement are sift_worker_main's);
 * set by the area that owns the kind when the library loads
 */
typedef void (*SiftWorkerMain) (Datum main_arg);
extern void sift_worker_define(SiftWorkerKind kind, SiftWorkerMain main);

/* The one entry point of every worker of sift */
extern PGDLLEXPORT void sift_worker_main(Datum main_arg);

/*
 * Registers the dynamic worker of kind for subject. main_arg is the argument its main gets; the database of the caller is the one the worker connects to. False when sift.max_maint_workers
 * workers are running (the count is one atomic in shared memory: no scan of the backends) or the server has no slot: one LOG line an hour for every job says so (sift: no background worker for ...).
 */
extern bool sift_worker_register(SiftWorkerKind kind, const char *subject, Datum main_arg);

/* The workers of the kinds above that run or are about to: with those of the queue (core/maint_sched.h) they are the workers sift.max_maint_workers allows */
extern int sift_worker_legacy_active(void);

#ifdef SIFT_TESTING
/* sift._test_worker_slots(n): this backend holds n slots of the workers of the kinds above, as many as sift.max_maint_workers allows (0: it gives them back); the number it holds */
extern int sift_worker_test_hold(int n);
#endif

/* Does the worker of this kind look for work again while it is up (the promotion and merge workers)? */
extern bool sift_worker_stays(SiftWorkerKind kind);

/* sift.max_maint_workers: the workers of sift that run at once in the server */
extern void sift_worker_init(void);

/* The job of this process when it is a worker of sift, else SIFT_JOB_NONE; and whether the writes of its steps are paced (every job but the rollup refresh) */
extern SiftJob sift_worker_job(void);
extern bool sift_worker_paced(void);

/* The tier of the steps of this worker (core/maint_pacer.h: 0 is P0, the first to be served and the last to give way); 0 for a process that is no worker */
extern int	sift_worker_tier(void);

/* Does this worker leave after its step, because a request of a tier before its own found no free worker? */
extern bool sift_worker_should_yield(void);

/* The subject of this worker, as its name has it ("schema.index"; "database name" for a job of the database) */
extern const char *sift_worker_target(void);

/* A nap of a worker: idle in pg_stat_activity for its length */
extern void sift_worker_nap(long ms);

/* The metapage notes that the step of job that began at start ended well (sift.workers last_ok, last_job, last_ms) */
extern void sift_worker_ok(Relation index, SiftJob job, TimestampTz start);

#ifdef SIFT_TESTING
/* Testing: how long a registered worker that is not up keeps its slot, in ms (0 = the default of 30 seconds) */
extern PGDLLIMPORT int sift_test_worker_lease_ms;
#endif

#endif							/* SIFT_WORKER_H */
