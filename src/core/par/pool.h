/*
 * core/par/pool.h - the compute threads of a backend: POSIX threads that run pure C over memory the backend's own thread made, started the first time a statement asks for them.
 *
 * A thread of the pool never touches PostgreSQL: no palloc, no ereport, no buffer, no lock, no interrupt check. It has every signal blocked, so the signals of the server reach the thread of the
 * backend only. The backend thread owns every byte a job reads and writes, starts the job, and waits for it (sift_par_pool_wait) before it frees that memory or leaves by an error: a job is
 * always started inside PG_TRY and drained in PG_FINALLY (core/par/morsel.h does both). A FATAL error does not unwind the stack, so no PG_FINALLY runs there: the first job registers a
 * before_shmem_exit callback that stops the job and waits for it before ShutdownPostgres frees the memory the threads work in. The threads end with the backend (on_proc_exit), never in the postmaster.
 *
 * A job is a function that every helper thread it wakes runs once; the backend thread is participant 0 and runs its share itself.
 */
#ifndef SIFT_CORE_PAR_POOL_H
#define SIFT_CORE_PAR_POOL_H

#include "postgres.h"

#define SIFT_PAR_MAX_THREADS	16	/* the backend's thread and its helpers */

/* Runs on helper thread worker (1 .. nthreads - 1) */
typedef void (*SiftParJobFn) (int worker, void *ctx);
/* Makes every fn of the job return, at its next unit of work: the backend calls it, and waits for the job, when it exits in the middle of one */
typedef void (*SiftParStopFn) (void *ctx);
/* Runs on a helper thread when it ends, for what the thread holds that only it can free (the decompression contexts of its thread) */
typedef void (*SiftParThreadFini) (void);

/* Whether this process may have pool threads: a backend, not a parallel worker (a thread of the pool and a fork of the postmaster never meet) */
extern bool sift_par_pool_usable(void);
/* The threads a statement may use, the backend's own included: max_parallel_workers_per_gather + 1, at most SIFT_PAR_MAX_THREADS; 1 when the pool is not usable here */
extern int	sift_par_pool_threads(void);

extern void sift_par_pool_on_thread_exit(SiftParThreadFini fn);

/* Wakes nthreads - 1 helpers (made now, once) to run fn(worker, ctx); a job is started only when the one before it was waited for */
extern void sift_par_pool_start(int nthreads, SiftParJobFn fn, SiftParStopFn stop, void *ctx);
/* Returns when every helper of the job has returned from fn: none touches the job's memory after that */
extern void sift_par_pool_wait(void);
extern bool sift_par_pool_busy(void);

#endif							/* SIFT_CORE_PAR_POOL_H */
