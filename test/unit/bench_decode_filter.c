/*
 * bench_decode_filter.c - the throughput of the filters of a granule on pool threads, one thread against four: the strings of the zstd granules of the fixture (URL-like and title-like text, the
 * block inflated to its strings, then LIKE '%lit%', NOT LIKE, equality) and its integers (a range on delta and frame-of-reference chunks), each worker in an arena of its own that it rewinds between
 * granules. The passes alternate 1T and 4T, and the report is the median of each: MB/s of the bytes the filter reads (the strings decoded, or the 8 bytes of a key), rows/s, and 4T/1T.
 *   make -C test/unit bench     (under flock /tmp/sift-perf.lock, optimized, no sanitizer)
 */
#include "postgres.h"

#include <stdatomic.h>
#include <time.h>

#include "col/compress.h"
#include "col/granule/cfilt_str_simd.h"
#include "col/granule/granule_filter.h"
#include "core/par/arena.h"
#include "core/cpu.h"
#include "granule_fixture.h"
#include "pgshim.h"
#include "unit.h"

#define GRANULES	60
#define REPEAT		60
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
} Work;

static const Work works[] = {
	{"url LIKE '%lit%' (zstd block)", "url", SIFT_COLOP_CONTAINS, "ka", true},
	{"url NOT LIKE '%lit%' (zstd block)", "url", SIFT_COLOP_NOTCONTAINS, "ra.com", true},
	{"title LIKE '%lit%' (FSST)", "title", SIFT_COLOP_CONTAINS, "mi", true},
	{"title = lit (FSST)", "title", SIFT_COLOP_EQ, NULL, true},
	{"phrase = lit (dictionary)", "phrase", SIFT_COLOP_EQ, NULL, true},
	{"event_time range (delta)", "event_time", SIFT_COLOP_GE, NULL, false},
	{"counter = key (frame of reference)", "counter", SIFT_COLOP_EQ, NULL, false}
};

typedef struct Bench
{
	const GranuleFixture *fx;
	int			gran[GRANULES];
	int			ngran;
	const Work *work;
	SiftColPred pred;
	SiftColValue val;
	SiftCfiltPred cp;
	int			col;
	uint64		bytes_per_pass;
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
	uint64		total = (uint64) b->ngran * REPEAT;

	pthread_barrier_wait(&b->go);
	for (uint64 i; (i = atomic_fetch_add(&b->next, 1)) < total;)
	{
		const SiftGranuleView *v = &b->fx->g[b->gran[i % b->ngran]].view;
		SiftStrMemo memo = {0};
		SiftErr		err;
		SiftCfiltResult res;
		uint64		mask[SIFT_CFILT_WORDS];

		sift_arena_reset(a);
		CHECK(sift_granule_filter_compressed_live_a(a, &err, &memo, v, &b->cp, NULL, mask, &res));
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
work_prepare(Bench *b, const Work *wk)
{
	const GranuleFixture *fx = b->fx;
	const FxRef *ref;
	int			g = b->gran[0];

	b->work = wk;
	b->col = granule_fixture_col(fx, wk->col);
	ref = &fx->g[g].ref[b->col];
	if (wk->strs && wk->lit != NULL)
		b->val = (SiftColValue) {.v.s = wk->lit,.len = strlen(wk->lit)};
	else if (wk->strs)
	{
		int			k = 0;

		while (ref->isnull[k])
			k++;
		b->val = (SiftColValue) {.v.s = ref->s[k].p,.len = ref->s[k].len};
	}
	else
		b->val.v.i = ref->i[FX_ROWS / 2];
	b->pred = (SiftColPred) {.col = b->col,.op = wk->op,.nvals = 1,.vals = &b->val};
	CHECK(sift_cfilt_compile(&b->cp, &fx->defs[b->col], &b->pred));
	b->bytes_per_pass = 0;
	for (int i = 0; i < b->ngran; i++)
	{
		const FxRef *r = &fx->g[b->gran[i]].ref[b->col];

		for (int k = 0; k < FX_ROWS; k++)
			b->bytes_per_pass += wk->strs ? (r->isnull[k] ? 0 : r->s[k].len) : 8;
	}
	b->bytes_per_pass *= REPEAT;
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
	for (int g = 0; g < GRANULES; g++)
		if (strncmp(b.fx->g[g].cfg->name, "cold-zstd", 9) == 0)
			b.gran[b.ngran++] = g;
	for (int t = 0; t < THREADS; t++)
		b.arena[t] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	printf("bench_decode_filter: %d zstd granules x %d rows, x%d per pass, %d passes, %s level\n", b.ngran, FX_ROWS, REPEAT, PASSES,
		   sift_cpu.active == SIFT_SIMD_AVX512 ? "avx512" : sift_cpu.active == SIFT_SIMD_AVX2 ? "avx2" : "scalar");
	load_line("start");
	for (size_t k = 0; k < lengthof(works); k++)
	{
		double		t1[PASSES],
					t4[PASSES];

		work_prepare(&b, &works[k]);
		run_pass(&b, 1);
		for (int p = 0; p < PASSES; p++)
		{
			t1[p] = run_pass(&b, 1);
			t4[p] = run_pass(&b, THREADS);
		}
		{
			double		m1 = median(t1, PASSES),
						m4 = median(t4, PASSES),
						rows = (double) b.ngran * REPEAT * FX_ROWS;

			printf("%-38s 1T %8.1f MB/s %7.2f Mrows/s | 4T %8.1f MB/s %7.2f Mrows/s | 4T/1T %.2fx%s\n", works[k].name, b.bytes_per_pass / m1 / 1e6, rows / m1 / 1e6,
				   b.bytes_per_pass / m4 / 1e6, rows / m4 / 1e6, m1 / m4, atomic_load(&b.declined) ? " (granules left to the decoder)" : "");
		}
	}
	load_line("end");
	sift_colz_thread_fini();
	granule_fixture_free((GranuleFixture *) b.fx);
	return 0;
}
