/*
 * core/par/pool.c - core/par: the radix streams, the top-K heap and its bound, the pool, and the morsel driver, on four threads that the trap shim flags as workers (no palloc, no ereport).
 */
#include "postgres.h"

#include <math.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/hash.h"
#include "core/par/morsel.h"
#include "core/par/pool.h"
#include "core/par/radix.h"
#include "core/par/topk.h"
#include "miscadmin.h"
#include "agg/par/host.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define WORKERS		4
#define SLAB		(8 << 20)

static uint64
mix(uint64 i)
{
	return sift_fmix64(i + 1);
}

/* ---- radix: a record is its value, the bytes that follow it (a multiple of 8) and those bytes, all equal to the low byte of the value ---- */

typedef struct RadixJob
{
	SiftRadixOut out[WORKERS];
	SiftArena	arena[WORKERS];
	uint32		per_worker;
} RadixJob;

static uint32
payload_of(uint64 i, uint64 v)
{
	return i % 5 == 0 ? 8 * (1 + v % 40) : 0;
}

static void
radix_scatter(void *arg, int t)
{
	RadixJob   *j = arg;

	for (uint32 i = 0; i < j->per_worker; i++)
	{
		const uint64 v = mix((uint64) t * j->per_worker + i);
		const uint64 n = payload_of(i, v);
		uint8	   *rec = sift_radix_reserve(&j->out[t], sift_radix_part(v), 16 + n);

		SIFT_CHECK(rec != NULL);
		memcpy(rec, &v, 8);
		memcpy(rec + 8, &n, 8);
		memset(rec + 16, (int) (v & 0xff), n);
	}
}

typedef struct RadixSeen
{
	uint64		sum;
	uint64		n;
	uint32		part;
} RadixSeen;

static bool
radix_chunk(void *arg, const uint8 *data, uint32 len)
{
	RadixSeen  *s = arg;

	for (uint32 off = 0; off < len;)
	{
		uint64		v,
					n;

		SIFT_CHECK(len - off >= 16);
		memcpy(&v, data + off, 8);
		memcpy(&n, data + off + 8, 8);
		SIFT_CHECK(sift_radix_part(v) == s->part && len - off >= 16 + n);
		for (uint64 b = 0; b < n; b++)
			SIFT_CHECK(data[off + 16 + b] == (uint8) (v & 0xff));
		s->sum += v;
		s->n++;
		off += 16 + n;
	}
	return true;
}

static void
test_radix_scatter(void)
{
	static RadixJob job;
	uint8	   *slab[WORKERS];
	uint64		expect = 0;
	RadixSeen	all = {0};

	job.per_worker = 10000;
	for (int t = 0; t < WORKERS; t++)
	{
		slab[t] = aligned_alloc(SIFT_ARENA_ALIGN, SLAB);
		SIFT_CHECK(slab[t] != NULL);
		sift_arena_init(&job.arena[t], slab[t], SLAB);
		sift_radix_init(&job.out[t], &job.arena[t]);
	}
	unit_run_workers(WORKERS, radix_scatter, &job);
	for (uint64 i = 0; i < (uint64) WORKERS * job.per_worker; i++)
		expect += mix(i);
	for (uint32 p = 0; p < SIFT_RADIX_PARTS; p++)
		for (int t = 0; t < WORKERS; t++)
		{
			RadixSeen	s = {.part = p};

			SIFT_CHECK(sift_radix_scan(&job.out[t], p, radix_chunk, &s));
			SIFT_CHECK(s.n == sift_radix_nrec(&job.out[t], p));
			all.sum += s.sum;
			all.n += s.n;
		}
	SIFT_CHECK(all.n == (uint64) WORKERS * job.per_worker && all.sum == expect);
	for (int t = 0; t < WORKERS; t++)
		free(slab[t]);
}

static bool
radix_stop(void *arg, const uint8 *data, uint32 len)
{
	return false;
}

static void
test_radix_full_arena(void)
{
	static uint8 slab[8192] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftArena	a;
	SiftRadixOut o;
	const uint64 v = 7;
	uint64		n;

	sift_arena_init(&a, slab, sizeof(slab));
	sift_radix_init(&o, &a);
	for (int i = 0; i < 8; i++)
		SIFT_CHECK(sift_radix_put(&o, 3, &v, 8));
	SIFT_CHECK(sift_radix_nrec(&o, 3) == 8);
	SIFT_CHECK(sift_radix_reserve(&o, 3, 60000) == NULL && sift_radix_nrec(&o, 3) == 8);
	SIFT_CHECK(!sift_radix_scan(&o, 3, radix_stop, NULL));
	while (sift_radix_put(&o, 5, &v, 8))
		;
	n = sift_radix_nrec(&o, 5);
	SIFT_CHECK(n > 0 && !sift_radix_put(&o, 5, &v, 8) && sift_radix_nrec(&o, 5) == n && a.used <= a.cap);
	SIFT_CHECK(sift_radix_scan(&o, 0, radix_stop, NULL));
}

/* ---- top-K: the K best by metric, then by the key (a total order), from four heaps, equal to the first K of a sort of everything ---- */

typedef struct TkElem
{
	int64		metric;
	uint64		key;
	char	   *txt;
	uint32		len;
} TkElem;

typedef struct TkJob
{
	SiftParTopK tk[WORKERS];
	SiftArena	arena[WORKERS];
	SiftAtomicBound bound;
	uint32		n;
	uint32		k;
} TkJob;

static bool
tk_better(const void *a, const void *b, const void *ctx)
{
	const TkElem *x = a,
			   *y = b;

	if (x->metric != y->metric)
		return x->metric > y->metric;
	return x->key < y->key;
}

static bool
tk_store(void *dst, const void *cand, SiftArena *str, const void *ctx)
{
	const TkElem *c = cand;
	TkElem	   *d = dst;
	char	   *buf = sift_arena_alloc(str, c->len, 1);

	if (buf == NULL)
		return false;
	memcpy(buf, c->txt, c->len);
	*d = *c;
	d->txt = buf;
	return true;
}

static void
tk_value(uint64 i, TkElem *e, char *txt)
{
	const uint64 v = mix(i);

	e->metric = (int64) (v % 50);
	e->key = i;
	e->len = 1 + v % 12;
	memset(txt, 'a' + (int) (i % 26), e->len);
	e->txt = txt;
}

static void
tk_offer(void *arg, int t)
{
	TkJob	   *j = arg;

	for (uint32 i = t; i < j->n; i += WORKERS)
	{
		TkElem		e;
		char		txt[16];

		tk_value(i, &e, txt);
		if (sift_bound_key_i64(e.metric, true) < sift_bound_load(&j->bound))
			continue;
		SIFT_CHECK(sift_par_topk_offer(&j->tk[t], &e));
		if (sift_par_topk_worst(&j->tk[t]) != NULL)
			sift_bound_raise(&j->bound, sift_bound_key_i64(((const TkElem *) sift_par_topk_worst(&j->tk[t]))->metric, true));
	}
}

static int
tk_cmp_ref(const void *a, const void *b)
{
	return tk_better(a, b, NULL) ? -1 : tk_better(b, a, NULL) ? 1 : 0;
}

static void
test_topk(uint32 n, uint32 k)
{
	static TkJob job;
	static uint8 slab[WORKERS][1 << 20] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftArena	out;
	static uint8 outslab[1 << 20] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftParTopK *heaps[WORKERS];
	TkElem	   *ref = malloc(sizeof(TkElem) * n);
	const void **sorted;
	uint32		got;

	job.n = n;
	job.k = k;
	sift_bound_init(&job.bound, 0);
	for (int t = 0; t < WORKERS; t++)
	{
		sift_arena_init(&job.arena[t], slab[t], sizeof(slab[t]));
		SIFT_CHECK(sift_par_topk_init(&job.tk[t], &job.arena[t], k, sizeof(TkElem), 64 << 10, tk_better, tk_store, NULL));
		heaps[t] = &job.tk[t];
	}
	unit_run_workers(WORKERS, tk_offer, &job);
	for (uint32 i = 0; i < n; i++)
	{
		static char txt[1 << 16][16];

		tk_value(i, &ref[i], txt[i % (1 << 16)]);
		ref[i].txt = NULL;
	}
	qsort(ref, n, sizeof(TkElem), tk_cmp_ref);
	sift_arena_init(&out, outslab, sizeof(outslab));
	SIFT_CHECK(sift_par_topk_merge(heaps, WORKERS, &out, &sorted, &got));
	SIFT_CHECK(got == Min(k, n));
	for (uint32 i = 0; i < got; i++)
	{
		const TkElem *e = sorted[i];

		SIFT_CHECK(e->metric == ref[i].metric && e->key == ref[i].key);
		SIFT_CHECK(e->len == 1 + mix(e->key) % 12 && e->txt[0] == 'a' + (int) (e->key % 26));
	}
	free(ref);
}

static void
test_bound_keys(void)
{
	static const int64 vals[] = {INT64_MIN, -5, -1, 0, 1, 7, INT64_MAX};
	static const float fl[] = {-INFINITY, -1e30f, -2.5f, -0.0f, 0.0f, 1e-30f, 3.5f, 1e30f, INFINITY};
	SiftAtomicBound b;

	for (int i = 1; i < 7; i++)
		SIFT_CHECK(sift_bound_key_i64(vals[i - 1], true) < sift_bound_key_i64(vals[i], true) &&
			  sift_bound_key_i64(vals[i - 1], false) > sift_bound_key_i64(vals[i], false));
	for (int i = 1; i < 9; i++)
		SIFT_CHECK(sift_bound_key_f32(fl[i - 1]) <= sift_bound_key_f32(fl[i]));
	for (int i = 0; i < 9; i++)
		SIFT_CHECK(sift_bound_f32(sift_bound_key_f32(fl[i])) == fl[i]);
	sift_bound_init(&b, 10);
	sift_bound_raise(&b, 5);
	SIFT_CHECK(sift_bound_load(&b) == 10);
	sift_bound_raise(&b, 11);
	SIFT_CHECK(sift_bound_load(&b) == 11);
}

/* ---- pool: jobs on helper threads that have every signal blocked, cancel, the drain of a FATAL exit, the end of the threads ---- */

extern int	max_parallel_workers_per_gather;
extern bool sift_parallel_scan;
extern bool IsUnderPostmaster;
extern int	ParallelWorkerNumber;

typedef struct PoolJob
{
	atomic_int	ran[SIFT_PAR_MAX_THREADS];
	atomic_int	stop;
	atomic_int	started;
	atomic_int	blocked;
	atomic_int	faults_open;
} PoolJob;

static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGSYS, SIGTRAP};

static bool
faults_unblocked(const sigset_t *cur)
{
	for (size_t i = 0; i < sizeof(fault_signals) / sizeof(fault_signals[0]); i++)
		if (sigismember(cur, fault_signals[i]))
			return false;
	return true;
}

static void
job_count(int worker, void *ctx)
{
	PoolJob    *j = ctx;
	sigset_t	cur;

	sift_test_in_worker = true;
	pthread_sigmask(SIG_BLOCK, NULL, &cur);
	if (sigismember(&cur, SIGINT) && sigismember(&cur, SIGTERM) && sigismember(&cur, SIGUSR1) && sigismember(&cur, SIGALRM))
		atomic_fetch_add(&j->blocked, 1);
	if (faults_unblocked(&cur))
		atomic_fetch_add(&j->faults_open, 1);
	atomic_fetch_add(&j->ran[worker], 1);
}

static void
job_spin(int worker, void *ctx)
{
	PoolJob    *j = ctx;

	sift_test_in_worker = true;
	atomic_fetch_add(&j->started, 1);
	while (!atomic_load(&j->stop))
		;
	atomic_fetch_add(&j->ran[worker], 1);
}

static void
job_stop(void *ctx)
{
	atomic_store(&((PoolJob *) ctx)->stop, 1);
}

static int	fini_calls;

static void
count_fini(void)
{
	__atomic_fetch_add(&fini_calls, 1, __ATOMIC_RELAXED);
}

static void
test_pool_threads(void)
{
	max_parallel_workers_per_gather = 3;
	SIFT_CHECK(sift_par_pool_usable() && sift_par_pool_threads() == 4);
	max_parallel_workers_per_gather = 0;
	SIFT_CHECK(sift_par_pool_threads() == 1);
	max_parallel_workers_per_gather = 40;
	SIFT_CHECK(sift_par_pool_threads() == SIFT_PAR_MAX_THREADS);
	ParallelWorkerNumber = 0;
	SIFT_CHECK(!sift_par_pool_usable() && sift_par_pool_threads() == 1);
	ParallelWorkerNumber = -1;
	IsUnderPostmaster = false;
	SIFT_CHECK(!sift_par_pool_usable() && sift_par_pool_threads() == 1);
	IsUnderPostmaster = true;
	max_parallel_workers_per_gather = 3;
}

/* the settings that turn parallelism down turn the threads of the backend down: no worker may run, none is made */
static void
test_pool_settings(void)
{
	max_parallel_workers_per_gather = 8;
	sift_parallel_scan = false;
	SIFT_CHECK(sift_par_pool_threads() == 1);
	sift_parallel_scan = true;
	max_parallel_workers = 0;
	SIFT_CHECK(sift_par_pool_threads() == 1);
	max_parallel_workers = 2;
	SIFT_CHECK(sift_par_pool_threads() == 3);
	max_parallel_workers = 64;
	SIFT_CHECK(sift_par_pool_threads() == 9);
	max_parallel_workers_per_gather = 3;
}

/* the signals the hardware raises in the thread that faults are never blocked in a thread of the pool: blocking them is undefined, and a fault would hang or take the process without a report */
static void
test_pool_faults(void)
{
	static PoolJob j;

	for (int n = 2; n <= 4; n++)
	{
		memset(&j, 0, sizeof(j));
		sift_par_pool_start(n, job_count, job_stop, &j);
		sift_par_pool_wait();
		SIFT_CHECK(atomic_load(&j.faults_open) == n - 1);
	}
}

/* a job in flight owns the pool: what runs on the leader meanwhile (an operator's callback) gets one thread, never a second job */
static void
test_pool_nested(void)
{
	static PoolJob j;

	memset(&j, 0, sizeof(j));
	sift_par_pool_start(4, job_spin, job_stop, &j);
	SIFT_CHECK(sift_par_pool_threads() == 1);
	job_stop(&j);
	sift_par_pool_wait();
	SIFT_CHECK(sift_par_pool_threads() == 4);
}

/* a process forked while the pool has threads (COPY ... PROGRAM, a procedural language) has none of them: its pool is idle, and makes its own */
#ifndef __SANITIZE_THREAD__
static void
test_pool_fork(void)
{
	static PoolJob j;
	int			status;
	pid_t		pid;

	memset(&j, 0, sizeof(j));
	sift_par_pool_start(4, job_spin, job_stop, &j);
	while (atomic_load(&j.started) < 3)
		;
	pid = fork();
	SIFT_CHECK(pid >= 0);
	if (pid == 0)
	{
		alarm(20);
		memset(&j, 0, sizeof(j));
		SIFT_CHECK(!sift_par_pool_busy());
		sift_par_pool_start(3, job_count, job_stop, &j);
		sift_par_pool_wait();
		SIFT_CHECK(atomic_load(&j.ran[1]) == 1 && atomic_load(&j.ran[2]) == 1);
		par_host_before_shmem_exit();
		par_host_proc_exit();
		_exit(0);
	}
	job_stop(&j);
	sift_par_pool_wait();
	SIFT_CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif

static void
test_pool_jobs(void)
{
	static PoolJob j;
	sigset_t	cur;

	sift_par_pool_on_thread_exit(count_fini);
	sift_par_pool_on_thread_exit(count_fini);
	for (int round = 0; round < 2000; round++)
	{
		const int	n = 2 + round % 3;

		memset(&j, 0, sizeof(j));
		sift_par_pool_start(n, job_count, job_stop, &j);
		SIFT_CHECK(sift_par_pool_busy());
		sift_par_pool_wait();
		SIFT_CHECK(!sift_par_pool_busy());
		SIFT_CHECK(atomic_load(&j.ran[0]) == 0);
		for (int w = 1; w < SIFT_PAR_MAX_THREADS; w++)
			SIFT_CHECK(atomic_load(&j.ran[w]) == (w < n ? 1 : 0));
		SIFT_CHECK(atomic_load(&j.blocked) == n - 1);
	}
	pthread_sigmask(SIG_BLOCK, NULL, &cur);
	SIFT_CHECK(!sigismember(&cur, SIGINT) && !sigismember(&cur, SIGTERM));
}

static void
test_pool_stop(void)
{
	static PoolJob j;

	for (int round = 0; round < 200; round++)
	{
		memset(&j, 0, sizeof(j));
		sift_par_pool_start(4, job_spin, job_stop, &j);
		while (atomic_load(&j.started) < 3)
			;
		job_stop(&j);
		sift_par_pool_wait();
		SIFT_CHECK(atomic_load(&j.ran[1]) == 1 && atomic_load(&j.ran[2]) == 1 && atomic_load(&j.ran[3]) == 1);
		memset(&j, 0, sizeof(j));
		sift_par_pool_start(4, job_count, job_stop, &j);
		sift_par_pool_wait();
		SIFT_CHECK(atomic_load(&j.ran[3]) == 1);
	}
}

static void
test_pool_exit(void)
{
	static PoolJob j;

	memset(&j, 0, sizeof(j));
	sift_par_pool_start(4, job_spin, job_stop, &j);
	while (atomic_load(&j.started) < 3)
		;
	par_host_before_shmem_exit();
	SIFT_CHECK(!sift_par_pool_busy() && atomic_load(&j.stop) == 1 && atomic_load(&j.ran[1]) == 1 && atomic_load(&j.ran[3]) == 1);
	par_host_proc_exit();
	SIFT_CHECK(__atomic_load_n(&fini_calls, __ATOMIC_RELAXED) == 3);
}

/* ---- driver: every item scatters into the partitions in phase 1, each partition is summed in phase 2; a failure, an overflow and a cancel in the middle of a run ---- */

#define ITEMS		(1u << 21)

typedef struct SumCtx
{
	SiftParArenas *ar;
	SiftRadixOut out[SIFT_PAR_MAX_THREADS];
	uint64		psum[SIFT_RADIX_PARTS];
	uint64		pcnt[SIFT_RADIX_PARTS];
	uint64		wfold[SIFT_PAR_MAX_THREADS];
	atomic_int	ran[SIFT_PAR_MAX_THREADS];
	uint32		fail_at;
	uint32		fail_part;
	SiftErrCode fail_code;
	uint32		cancel_at;
	uint32		leader_morsels;
	uint32		drain_at;
	bool		finished;
	bool		drained;
	uint64		left_items;
} SumCtx;

static bool
sum_phase1(void *vc, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	SumCtx	   *c = vc;

	sift_test_in_worker = t != 0;
	atomic_store(&c->ran[t], 1);
	SIFT_CHECK(scratch->used == 0);
	for (uint32 i = lo; i < hi; i++)
	{
		const uint64 v = mix(i);

		if (i == c->fail_at)
		{
			*done = i - lo;
			sift_err_fill(err, c->fail_code, i, 0, "phase 1 failed");
			return false;
		}
		SIFT_CHECK(sift_radix_put(&c->out[t], sift_radix_part(v), &v, 8));
		c->wfold[t]++;
	}
	SIFT_CHECK(sift_arena_alloc(scratch, 100, 8) != NULL);
	if (t == 0 && ++c->leader_morsels == c->cancel_at)
		InterruptPending = QueryCancelPending = true;
	if (t == 0 && c->leader_morsels == c->drain_at)
		par_host_before_shmem_exit();
	return true;
}

static bool
sum_seal(void *vc, uint32 *nparts, SiftErr *err)
{
	*nparts = SIFT_RADIX_PARTS;
	return true;
}

static bool
sum_chunk(void *arg, const uint8 *data, uint32 len)
{
	SumCtx	   *c = arg;
	uint64		v;

	for (uint32 off = 0; off < len; off += 8)
	{
		memcpy(&v, data + off, 8);
		c->psum[sift_radix_part(v)] += v;
		c->pcnt[sift_radix_part(v)]++;
	}
	return true;
}

static bool
sum_phase2(void *vc, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	SumCtx	   *c = vc;

	sift_test_in_worker = t != 0;
	SIFT_CHECK(work->used == 0 && sift_arena_alloc(work, 1000, 8) != NULL);
	if (part == c->fail_part)
	{
		sift_err_fill(err, SIFT_ERR_WORK_MEM, part, 0, "phase 2 failed");
		return false;
	}
	for (int v = 0; v < c->ar->n; v++)
		SIFT_CHECK(sift_radix_scan(&c->out[v], part, sum_chunk, c));
	return true;
}

static void
sum_finish(void *vc)
{
	((SumCtx *) vc)->finished = true;
}

static void
sum_drain(void *vc, const SiftParRange *left, uint32 nleft)
{
	SumCtx	   *c = vc;

	c->drained = true;
	for (uint32 i = 0; i < nleft; i++)
		c->left_items += left[i].hi - left[i].lo;
}

static const SiftParOps sum_ops = {.phase1 = sum_phase1,.seal = sum_seal,.phase2 = sum_phase2,.finish = sum_finish,.overflow_drain = sum_drain};

static void
sum_setup(SumCtx *c, uint32 nthreads)
{
	pgshim_reset();
	memset(c, 0, sizeof(*c));
	c->fail_at = c->cancel_at = c->drain_at = c->fail_part = UINT32_MAX;
	c->ar = sift_par_arenas_create(CurrentMemoryContext, nthreads, 64 << 10, (size_t) ITEMS * 16 + (1 << 20), 1 << 20);
	for (uint32 t = 0; t < nthreads; t++)
		sift_radix_init(&c->out[t], &c->ar->w[t].state);
}

static uint64
sum_folded(const SumCtx *c)
{
	uint64		n = 0;

	for (uint32 t = 0; t < c->ar->n; t++)
		n += c->wfold[t];
	return n;
}

typedef struct RunArgs
{
	SumCtx	   *c;
	SiftParCfg	cfg;
	bool		ok;
} RunArgs;

static void
run_it(void *arg)
{
	RunArgs    *a = arg;

	a->ok = sift_par_run(&a->cfg, &sum_ops, a->c, a->c->ar, NULL);
}

static void
test_run_sums(void)
{
	uint64		expect = 0;

	for (uint32 i = 0; i < ITEMS; i++)
		expect += mix(i);
	for (uint32 w = 1; w <= 4; w++)
	{
		static SumCtx c;
		RunArgs		a = {.c = &c,.cfg = {.nthreads = w,.items = ITEMS}};
		uint64		sum = 0,
					cnt = 0;

		sum_setup(&c, w);
		run_it(&a);
		SIFT_CHECK(a.ok && c.finished && !c.drained && !sift_par_pool_busy());
		for (int p = 0; p < SIFT_RADIX_PARTS; p++)
		{
			sum += c.psum[p];
			cnt += c.pcnt[p];
		}
		SIFT_CHECK(cnt == ITEMS && sum == expect && sum_folded(&c) == ITEMS);
		for (uint32 t = w; t < SIFT_PAR_MAX_THREADS; t++)
			SIFT_CHECK(!atomic_load(&c.ran[t]));
	}
}

static void
test_run_failures(void)
{
	static SumCtx c;
	RunArgs		a = {.c = &c,.cfg = {.nthreads = 4,.items = ITEMS}};

	sum_setup(&c, 4);
	c.fail_at = ITEMS / 2;
	c.fail_code = SIFT_ERR_CORRUPT;
	SIFT_CHECK(!pgshim_try(run_it, &a) && pgshim_sqlstate != 0 && !c.finished && !sift_par_pool_busy());

	sum_setup(&c, 4);
	c.fail_at = ITEMS / 3;
	c.fail_code = SIFT_ERR_WORK_MEM;
	run_it(&a);
	SIFT_CHECK(!a.ok && c.drained && !c.finished && !sift_par_pool_busy());
	SIFT_CHECK(sum_folded(&c) + c.left_items == ITEMS && c.left_items > 0);

	sum_setup(&c, 4);
	c.fail_part = 5;
	run_it(&a);
	SIFT_CHECK(!a.ok && c.drained && c.left_items == 0 && sum_folded(&c) == ITEMS && !sift_par_pool_busy());

	sum_setup(&c, 1);
	a.cfg.nthreads = 1;
	c.fail_at = ITEMS / 5;
	c.fail_code = SIFT_ERR_WORK_MEM;
	run_it(&a);
	SIFT_CHECK(!a.ok && c.drained && sum_folded(&c) + c.left_items == ITEMS);
}

/* the exit of a backend (FATAL) in the middle of a run: the pool is stopped and waited for where the leader stands */
static void
test_run_drain(void)
{
	static SumCtx c;
	RunArgs		a = {.c = &c,.cfg = {.nthreads = 4,.items = ITEMS}};

	for (int round = 0; round < 50; round++)
	{
		sum_setup(&c, 4);
		c.drain_at = 1000 * (1 + round % 4);
		run_it(&a);
		SIFT_CHECK(!sift_par_pool_busy() && sum_folded(&c) < ITEMS);
	}
}

/* ThreadSanitizer does not follow siglongjmp (PG_TRY): the cancel of the leader is run by the other builds */
#ifndef __SANITIZE_THREAD__
static void
test_run_cancel(void)
{
	static SumCtx c;
	RunArgs		a = {.c = &c,.cfg = {.nthreads = 4,.items = ITEMS}};

	for (int round = 0; round < 50; round++)
	{
		bool		caught = false;

		sum_setup(&c, 4);
		c.cancel_at = 1000 * (1 + round % 4);
		PG_TRY();
		{
			run_it(&a);
		}
		PG_CATCH();
		{
			caught = true;
		}
		PG_END_TRY();
		SIFT_CHECK(caught);
		SIFT_CHECK(!c.finished && !sift_par_pool_busy());
		SIFT_CHECK(sum_folded(&c) < ITEMS);
	}
	sum_setup(&c, 4);
	run_it(&a);
	SIFT_CHECK(a.ok && c.finished);
}
#endif

int
main(void)
{
	pgshim_keep();
	test_radix_scatter();
	test_radix_full_arena();
	test_bound_keys();
	test_topk(200000, 10);
	test_topk(200000, 1000);
	test_topk(300, 1000);
	test_topk(5000, 1);
	test_pool_threads();
	if (unit_wants("pool_settings"))
		test_pool_settings();
	test_pool_jobs();
	test_pool_stop();
	if (unit_wants("pool_faults"))
		test_pool_faults();
	if (unit_wants("pool_nested"))
		test_pool_nested();
#ifndef __SANITIZE_THREAD__
	if (unit_wants("pool_fork"))
		test_pool_fork();
#endif
	test_run_sums();
	test_run_failures();
	test_run_drain();
#ifndef __SANITIZE_THREAD__
	test_run_cancel();
#endif
	test_pool_exit();
	puts("par_core: ok");
	return 0;
}
