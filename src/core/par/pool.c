/*
 * core/par/pool.c - the compute threads of a backend (core/par/pool.h).
 *
 * The pool is one static block: its threads are made with every signal blocked (the mask is inherited, the backend thread gets its own back at once), so a signal of the server is never taken by one
 * of them; they wait on a condition variable for a job and on another for the end of it, never on a timer. on_proc_exit joins them, so a backend leaves no thread behind.
 */
#include "postgres.h"

#include <pthread.h>
#include <signal.h>

#include "access/parallel.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "storage/ipc.h"

#include "core/par/pool.h"

#define POOL_STACK_BYTES	(1024 * 1024)
#define POOL_FINI_MAX		4

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
	return sift_par_pool_usable() ? Min(max_parallel_workers_per_gather, SIFT_PAR_MAX_THREADS - 1) + 1 : 1;
}

void
sift_par_pool_on_thread_exit(SiftParThreadFini fn)
{
	for (int i = 0; i < pool.nfini; i++)
		if (pool.fini[i] == fn)
			return;
	Assert(pool.nspawned == 0 && pool.nfini < POOL_FINI_MAX);
	pool.fini[pool.nfini++] = fn;
}

static void *
pool_main(void *p)
{
	const int	id = *(const int *) p;
	uint64		seen = 0;

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
	pthread_mutex_unlock(&pool.mu);
	for (int i = 0; i < pool.nfini; i++)
		pool.fini[i]();
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

static void
pool_spawn(int helpers)
{
	pthread_attr_t attr;
	sigset_t	all,
				old;

	if (!pool.exit_registered)
	{
		before_shmem_exit(pool_drain, 0);
		on_proc_exit(pool_exit, 0);
		pool.exit_registered = true;
	}
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, POOL_STACK_BYTES);
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	for (; pool.nspawned < helpers; pool.nspawned++)
	{
		worker_id[pool.nspawned] = pool.nspawned + 1;
		if (pthread_create(&pool.thread[pool.nspawned], &attr, pool_main, &worker_id[pool.nspawned]) != 0)
			break;
	}
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	pthread_attr_destroy(&attr);
	if (pool.nspawned < helpers)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not start the compute threads of the backend")));
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
