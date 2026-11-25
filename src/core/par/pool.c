/*
 * core/par/pool.c - the compute threads of a backend (core/par/pool.h).
 *
 * The pool is one static block: its threads are made with every signal blocked but the ones the hardware raises in the thread that faults (the mask is inherited, the backend thread gets its own
 * back at once), so a signal of the server is never taken by one of them; they wait on a condition variable for a job and on another for the end of it, never on a timer. on_proc_exit joins them,
 * so a backend leaves no thread behind, and a process forked from the backend (a program, a procedural language) starts with an idle pool of its own.
 */
#include "postgres.h"

#include <pthread.h>
#include <signal.h>

#include "access/parallel.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "storage/ipc.h"

#include "core/par/pool.h"
#include "core/sift.h"

#define POOL_STACK_BYTES	(1024 * 1024)
#define POOL_FINI_MAX		4

static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGSYS, SIGTRAP};

typedef struct SiftParPool
{
	pthread_mutex_t mu;
	pthread_cond_t cv_work;
	pthread_cond_t cv_done;
	pthread_t	thread[SIFT_PAR_MAX_THREADS - 1];
	int			nspawned;
	bool		exit_registered;
	bool		shutdown;
	uint64		epoch;
	SiftParJobFn fn;
	SiftParStopFn stop;
	void	   *ctx;
	int			active_n;
	int			finished_n;
	bool		busy;
	SiftParThreadFini fini[POOL_FINI_MAX];
	int			nfini;
} SiftParPool;

static SiftParPool pool = {.mu = PTHREAD_MUTEX_INITIALIZER,.cv_work = PTHREAD_COND_INITIALIZER,.cv_done = PTHREAD_COND_INITIALIZER};

static int	worker_id[SIFT_PAR_MAX_THREADS - 1];

bool
sift_par_pool_usable(void)
{
	return IsUnderPostmaster && MyBackendType == B_BACKEND && !IsParallelWorker();
}

int
sift_par_pool_threads(void)
{
	if (!sift_par_pool_usable() || pool.busy || !sift_parallel_scan || max_parallel_workers <= 0)
		return 1;
	return Min(max_parallel_workers_per_gather, Min(max_parallel_workers, SIFT_PAR_MAX_THREADS - 1)) + 1;
}

void
sift_par_pool_on_thread_exit(SiftParThreadFini fn)
{
	bool		known = false;

	pthread_mutex_lock(&pool.mu);
	for (int i = 0; i < pool.nfini; i++)
		known |= pool.fini[i] == fn;
	if (!known)
	{
		Assert(pool.nfini < POOL_FINI_MAX);
		pool.fini[pool.nfini++] = fn;
	}
	pthread_mutex_unlock(&pool.mu);
}

static void *
pool_main(void *p)
{
	const int	id = *(const int *) p;
	SiftParThreadFini fini[POOL_FINI_MAX];
	uint64		seen = 0;
	int			nfini;

	pthread_mutex_lock(&pool.mu);
	for (;;)
	{
		SiftParJobFn fn;
		void	   *ctx;

		while (!pool.shutdown && !(pool.epoch != seen && id < pool.active_n))
			pthread_cond_wait(&pool.cv_work, &pool.mu);
		if (pool.shutdown)
			break;
		seen = pool.epoch;
		fn = pool.fn;
		ctx = pool.ctx;
		pthread_mutex_unlock(&pool.mu);
		fn(id, ctx);
		pthread_mutex_lock(&pool.mu);
		if (++pool.finished_n == pool.active_n - 1)
			pthread_cond_broadcast(&pool.cv_done);
	}
	nfini = pool.nfini;
	memcpy(fini, pool.fini, sizeof(fini));
	pthread_mutex_unlock(&pool.mu);
	for (int i = 0; i < nfini; i++)
		fini[i]();
	return NULL;
}

/* before_shmem_exit callbacks run last registered first: this one stops the job before ShutdownPostgres frees the memory the threads work in */
static void
pool_drain(int code, Datum arg)
{
	if (!pool.busy)
		return;
	pool.stop(pool.ctx);
	sift_par_pool_wait();
}

static void
pool_exit(int code, Datum arg)
{
	pthread_mutex_lock(&pool.mu);
	pool.shutdown = true;
	pthread_cond_broadcast(&pool.cv_work);
	pthread_mutex_unlock(&pool.mu);
	for (int i = 0; i < pool.nspawned; i++)
		pthread_join(pool.thread[i], NULL);
	pool.nspawned = 0;
}

/* A forked process has none of the threads of the pool: its pool is idle, and the lock a thread of the parent held at the fork is free */
static void
pool_fork_prepare(void)
{
	pthread_mutex_lock(&pool.mu);
}

static void
pool_fork_parent(void)
{
	pthread_mutex_unlock(&pool.mu);
}

static void
pool_fork_child(void)
{
	pool.mu = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
	pool.cv_work = (pthread_cond_t) PTHREAD_COND_INITIALIZER;
	pool.cv_done = (pthread_cond_t) PTHREAD_COND_INITIALIZER;
	pool.nspawned = 0;
	pool.active_n = 0;
	pool.finished_n = 0;
	pool.busy = false;
}

/* Makes the helpers up to helpers; 0, or the error of the system */
static int
pool_create(int helpers)
{
	pthread_attr_t attr;
	sigset_t	mask,
				old;
	int			rc = pthread_attr_init(&attr);

	if (rc != 0)
		return rc;
	if ((rc = pthread_attr_setstacksize(&attr, POOL_STACK_BYTES)) == 0)
	{
		sigfillset(&mask);
		for (size_t i = 0; i < lengthof(fault_signals); i++)
			sigdelset(&mask, fault_signals[i]);
		pthread_sigmask(SIG_SETMASK, &mask, &old);
		while (rc == 0 && pool.nspawned < helpers)
		{
			worker_id[pool.nspawned] = pool.nspawned + 1;
			rc = pthread_create(&pool.thread[pool.nspawned], &attr, pool_main, &worker_id[pool.nspawned]);
			if (rc == 0)
				pool.nspawned++;
		}
		pthread_sigmask(SIG_SETMASK, &old, NULL);
	}
	pthread_attr_destroy(&attr);
	return rc;
}

static void
pool_spawn(int helpers)
{
	int			rc;

	if (!pool.exit_registered)
	{
		before_shmem_exit(pool_drain, 0);
		on_proc_exit(pool_exit, 0);
		pthread_atfork(pool_fork_prepare, pool_fork_parent, pool_fork_child);
		pool.exit_registered = true;
	}
	if ((rc = pool_create(helpers)) != 0)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not start the compute threads of the backend"), errdetail("%s", strerror(rc))));
}

void
sift_par_pool_start(int nthreads, SiftParJobFn fn, SiftParStopFn stop, void *ctx)
{
	Assert(sift_par_pool_usable() && !pool.busy && nthreads >= 2 && nthreads <= SIFT_PAR_MAX_THREADS);
	if (pool.nspawned < nthreads - 1)
		pool_spawn(nthreads - 1);
	pthread_mutex_lock(&pool.mu);
	pool.fn = fn;
	pool.stop = stop;
	pool.ctx = ctx;
	pool.active_n = nthreads;
	pool.finished_n = 0;
	pool.epoch++;
	pool.busy = true;
	pthread_cond_broadcast(&pool.cv_work);
	pthread_mutex_unlock(&pool.mu);
}

void
sift_par_pool_wait(void)
{
	pthread_mutex_lock(&pool.mu);
	while (pool.finished_n < pool.active_n - 1)
		pthread_cond_wait(&pool.cv_done, &pool.mu);
	pool.busy = false;
	pthread_mutex_unlock(&pool.mu);
}

bool
sift_par_pool_busy(void)
{
	return pool.busy;
}
