/*
 * bench_decode_filter.c - the throughput of the filters of a granule on pool threads, one thread against four: the strings of the zstd and lz4 granules of the fixture (URL-like and title-like text, the
 * block inflated to its strings, then LIKE '%lit%', NOT LIKE, equality) and its integers (a range on delta and frame-of-reference chunks), each worker in an arena of its own that it rewinds between
 * granules. The passes alternate 1T and 4T, and the report is the median of each: MB/s of the bytes the filter reads (the strings the filter reads, or the 8 bytes of a key), rows/s, and 4T/1T.
 *   make -C test/unit bench     (under flock /tmp/sift-perf.lock, optimized, no sanitizer)
 */
#include "postgres.h"

#include <stdatomic.h>
#include <time.h>

#include "col/compress.h"
#include "col/granule/cfilt/str_simd.h"
#include "col/granule/filter.h"
#include "core/par/arena.h"
#include "core/cpu.h"
#include "granule_fixture.h"
#include "pgshim.h"
#include "unit.h"

#define GRANULES	60
#define MIN_REPEAT	60
#define PASS_SECONDS	0.15
#define PASSES		9
#define THREADS		4
#define ARENA_BYTES	(2 * 1024 * 1024)

typedef struct Work
{
	const char *name;
	const char *col;
	SiftColOp	op;
	const char *lit;
	bool		strs;
	int			enc;			/* the chunks the filter runs on */
	int			alg;				/* ... in the granules of this wrapper (SIFT_CC_ALG_*), 0: of any */
} Work;

static const Work works[] = {
	{"url LIKE '%lit%' (zstd block)", "url", SIFT_COLOP_CONTAINS, "ka", true, SIFT_CENC_STR_BLOCK, SIFT_CC_ALG_ZSTD},
	{"url NOT LIKE '%lit%' (zstd block)", "url", SIFT_COLOP_NOTCONTAINS, "ra.com", true, SIFT_CENC_STR_BLOCK, SIFT_CC_ALG_ZSTD},
	{"url LIKE '%lit%' (lz4 block)", "url", SIFT_COLOP_CONTAINS, "ka", true, SIFT_CENC_STR_BLOCK, SIFT_CC_ALG_LZ4},
	{"title LIKE '%lit%' (FSST)", "title", SIFT_COLOP_CONTAINS, "mi", true, SIFT_CENC_STR_FSST, 0},
	{"title = lit (FSST)", "title", SIFT_COLOP_EQ, NULL, true, SIFT_CENC_STR_FSST, 0},
	{"phrase = lit (dictionary)", "phrase", SIFT_COLOP_EQ, NULL, true, SIFT_CENC_DICT_CODE, 0},
	{"event_time >= key (delta)", "event_time", SIFT_COLOP_GE, NULL, false, SIFT_CENC_INT_DELTA, 0},
	{"counter = key (frame of reference)", "counter", SIFT_COLOP_EQ, NULL, false, SIFT_CENC_INT_FOR, 0}
};

typedef struct Bench
{
	const GranuleFixture *fx;
	int			gran[GRANULES];
	int			ngran;
	const Work *work;
	SiftColPred pred[GRANULES];
	SiftColValue val[GRANULES];
	SiftCfiltPred cp[GRANULES];
	int			col;
	uint64		bytes_per_rep;			/* the bytes the filter reads in one repetition of every granule */
	uint64		repeat;				/* how many times a pass filters each granule: the pass lasts PASS_SECONDS or more */
	_Atomic uint64 next;
	_Atomic uint64 declined;
	SiftArena  *arena[THREADS];
	pthread_barrier_t go;
} Bench;

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int
cmp_double(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return (x > y) - (x < y);
}

static double
median(double *v, int n)
{
	qsort(v, n, sizeof(double), cmp_double);
	return v[n / 2];
}

static void
bench_worker(void *arg, int w)
{
	Bench	   *b = arg;
	SiftArena  *a = b->arena[w];
	uint64		total = (uint64) b->ngran * b->repeat;

	pthread_barrier_wait(&b->go);
	for (uint64 i; (i = atomic_fetch_add(&b->next, 1)) < total;)
	{
		const int	k = i % b->ngran;
		const SiftGranuleView *v = &b->fx->g[b->gran[k]].view;
		SiftStrMemo memo = {0};
		SiftErr		err;
		SiftCfiltResult res;
		uint64		mask[SIFT_CFILT_WORDS];

		sift_arena_reset(a);
		CHECK(sift_granule_filter_compressed_live_a(a, &err, &memo, v, &b->cp[k], NULL, mask, &res));
		atomic_fetch_add(&b->declined, res == SIFT_CFILT_UNSUPPORTED);
	}
	pthread_barrier_wait(&b->go);
	sift_colz_thread_fini();
}

static double
run_pass(Bench *b, int threads)
{
	double		t0;

	atomic_store(&b->next, 0);
	atomic_store(&b->declined, 0);
	CHECK(pthread_barrier_init(&b->go, NULL, threads + 1) == 0);
	{
		pthread_t	tid[THREADS];
		UnitWorkers w[THREADS];

		for (int i = 0; i < threads; i++)
		{
			w[i] = (UnitWorkers) {bench_worker, b, i};
			CHECK(pthread_create(&tid[i], NULL, unit_worker_main, &w[i]) == 0);
		}
		pthread_barrier_wait(&b->go);
		t0 = now_s();
		pthread_barrier_wait(&b->go);
		t0 = now_s() - t0;
		for (int i = 0; i < threads; i++)
			CHECK(pthread_join(tid[i], NULL) == 0);
	}
	pthread_barrier_destroy(&b->go);
	return t0;
}

static void
granules_pick(Bench *b, const Work *wk)
{
	b->ngran = 0;
	for (int g = 0; g < b->fx->ngranules; g++)
	{
		SiftChunk	c;

		sift_chunk_open(&b->fx->g[g].view, b->col + 1, &c);
		if (c.enc == wk->enc && (wk->alg == 0 || b->fx->g[g].cfg->alg == wk->alg))
			b->gran[b->ngran++] = g;
		sift_chunk_close(&c);
	}
	CHECK(b->ngran > 0);
}

/* The constant of the filter of granule k: the literal, or a value of the column in that granule, so that no granule is decided by its zone alone */
static void
predicate_make(Bench *b, int k)
{
	const Work *wk = b->work;
	const FxRef *ref = &b->fx->g[b->gran[k]].ref[b->col];
	SiftColValue *v = &b->val[k];
	int			r = FX_ROWS / 2;

	while (ref->isnull[r])
		r++;
	if (wk->strs)
		*v = wk->lit != NULL ? (SiftColValue) {.v.s = wk->lit,.len = strlen(wk->lit)} : (SiftColValue) {.v.s = ref->s[r].p,.len = ref->s[r].len};
	else
		v->v.i = ref->i[r];
	b->pred[k] = (SiftColPred) {.col = b->col,.op = wk->op,.nvals = 1,.vals = v};
	CHECK(sift_cfilt_compile(&b->cp[k], &b->fx->defs[b->col], &b->pred[k]));
}

static void
work_prepare(Bench *b, const Work *wk)
{
	b->work = wk;
	b->col = granule_fixture_col(b->fx, wk->col);
	granules_pick(b, wk);
	b->bytes_per_rep = 0;
	for (int k = 0; k < b->ngran; k++)
	{
		const FxRef *r = &b->fx->g[b->gran[k]].ref[b->col];

		predicate_make(b, k);
		for (int i = 0; i < FX_ROWS; i++)
			b->bytes_per_rep += wk->strs ? (r->isnull[i] ? 0 : r->s[i].len) : 8;
	}
	b->repeat = MIN_REPEAT;
}

static void
load_line(const char *what)
{
	char		buf[64] = "";
	FILE	   *f = fopen("/proc/loadavg", "r");

	if (f != NULL && fgets(buf, sizeof(buf), f) != NULL)
		buf[strcspn(buf, "\n")] = 0;
	if (f != NULL)
		fclose(f);
	printf("%s load average: %s\n", what, buf);
}

int
main(void)
{
	Bench		b = {0};

	granule_fixture_init();
	b.fx = granule_fixture_build(GRANULES, 11);
	for (int t = 0; t < THREADS; t++)
		b.arena[t] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	printf("bench_decode_filter: %d granules of %d rows, x%d or more per pass (a pass lasts %.2f s or more), %d passes, %s level\n", GRANULES, FX_ROWS, MIN_REPEAT, PASS_SECONDS, PASSES,
		   sift_cpu.active == SIFT_SIMD_AVX512 ? "avx512" : sift_cpu.active == SIFT_SIMD_AVX2 ? "avx2" : "scalar");
	load_line("start");
	for (size_t k = 0; k < lengthof(works); k++)
	{
		double		t1[PASSES],
					t4[PASSES];

		work_prepare(&b, &works[k]);
		b.repeat = MIN_REPEAT * (1 + (uint64) (PASS_SECONDS / run_pass(&b, 1)));
		run_pass(&b, 1);
		for (int p = 0; p < PASSES; p++)
		{
			t1[p] = run_pass(&b, 1);
			t4[p] = run_pass(&b, THREADS);
		}
		{
			double		m1 = median(t1, PASSES),
						m4 = median(t4, PASSES),
						bytes = (double) b.bytes_per_rep * b.repeat,
						rows = (double) b.ngran * b.repeat * FX_ROWS;

			printf("%-36s %2d granules 1T %9.1f MB/s %8.2f Mrows/s | 4T %9.1f MB/s %8.2f Mrows/s | 4T/1T %.2fx%s\n", works[k].name, b.ngran, bytes / m1 / 1e6, rows / m1 / 1e6,
				   bytes / m4 / 1e6, rows / m4 / 1e6, m1 / m4, atomic_load(&b.declined) ? " (granules left to the decoder)" : "");
		}
	}
	load_line("end");
	sift_colz_thread_fini();
	granule_fixture_free((GranuleFixture *) b.fx);
	return 0;
}
