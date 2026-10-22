/*
 * Aggregation operator throughput per level: ns per call over one vector of 4096 rows (and the speedup over scalar). Single thread
 * pinned to KT_CPU (default 2); run under flock /tmp/sift-perf.lock (make bench). Output is a markdown table.
 *
 * Inputs: random data, 50 % selection masks unless the row says otherwise (dens = fraction of rows selected). The bit vector and
 * integer column operations are slots of the column kernels (tools/kernels bench col).
 */
#define _GNU_SOURCE
#include "harness.h"

#include <sched.h>
#include <time.h>

#include "agg/kern_cases.h"

#define N			SIFT_AGG_VEC
#define NW			SIFT_AGG_VECW

/* Batches of >= 50 us, minimum over nbatch batches (AK_BATCHES to change), a compiler barrier after every evaluation */
#undef BENCH
#define BENCH(expr) BENCH_EX(expr, 5e4, nbatch, __asm__ volatile ("" ::: "memory"))

static SiftRng rng;
static int	nbatch = 200;
static const SiftAggKernels *levels[3];
static const char *lname[3];
static int	nlevels;
static volatile int64 sink;

static void *
arr(size_t bytes)
{
	uint8	   *p = AGK_ALLOC(bytes + SIFT_AGG_PAD);

	for (size_t i = 0; i < bytes + SIFT_AGG_PAD; i++)
		p[i] = (uint8) sift_rng_next(&rng);
	return p;
}

static uint64 *
make_mask(double dens)
{
	uint64	   *m = arr(NW * 8);

	memset(m, 0, NW * 8);
	for (int i = 0; i < N; i++)
		if (sift_rng_unit(&rng) < dens)
			m[i >> 6] |= UINT64CONST(1) << (i & 63);
	return m;
}

static void
report(const char *name, const double *t)
{
	printf("| %s | %.0f |", name, t[0]);
	for (int l = 1; l < nlevels; l++)
		printf(" %.0f | %.1fx |", t[l], t[0] / t[l]);
	printf("\n");
	fflush(stdout);
}

#define ROW(NAME, CALL) do { \
	double t_[3] = {0, 0, 0}; \
	for (int l_ = 0; l_ < nlevels; l_++) { const SiftAggKernels *k = levels[l_]; t_[l_] = BENCH(CALL); } \
	report(NAME, t_); \
} while (0)

int
main(void)
{
	cpu_set_t	set;
	int			cpu = getenv("KT_CPU") ? atoi(getenv("KT_CPU")) : 2;
	int32	   *i32,
			   *v2,
			   *k32;
	int64	   *i64;
	double	   *f64;
	uint32	   *code,
			   *runs,
			   *map,
			   *map2,
			   *gid,
			   *gid0,
			   *parent;
	uint64	   *out,
			   *rowmask,
			   *m50,
			   *mb;
	uint8	   *lut2;
	int64	   *cnt,
			   *iacc;
	double	   *facc;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
	kt_cpu_init();
	sift_aggkern_init();
	nbatch = getenv("AK_BATCHES") ? atoi(getenv("AK_BATCHES")) : nbatch;
	sift_rng_seed(&rng, 42);
	i32 = arr(N * 4);
	v2 = arr(N * 4);
	k32 = arr(N * 4);
	i64 = arr(N * 8);
	f64 = arr(N * 8);
	code = arr(N * 4);
	runs = arr(N * 4);
	map = arr(1024 * 4);
	map2 = arr(1000 * 4);
	gid = arr(N * 4);
	gid0 = arr(N * 4);
	parent = arr(N * 4);
	out = arr(NW * 8);
	rowmask = arr(NW * 8);
	m50 = make_mask(0.5);
	mb = make_mask(0.3);
	lut2 = arr(1000);
	cnt = arr(64 * 8);
	iacc = arr(64 * 8);
	facc = arr(64 * 8);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (l == 0 || sift_aggkern_for(l) != levels[nlevels - 1])
		{
			levels[nlevels] = sift_aggkern_for(l);
			lname[nlevels++] = kt_level_name[l];
		}

	for (int i = 0; i < N; i++)
	{
		i32[i] = sift_rng_below(&rng, 1000000);
		i64[i] = (int64) (sift_rng_next(&rng) >> 24);
		f64[i] = sift_rng_unit(&rng) * 1000;
		code[i] = sift_rng_below(&rng, 1024);
		runs[i] = i > 0 && sift_rng_below(&rng, 10) ? runs[i - 1] : sift_rng_below(&rng, 1024);
		v2[i] = 7 + sift_rng_below(&rng, 1200);
		k32[i] = -5 + sift_rng_below(&rng, 1000);
		gid[i] = sift_rng_below(&rng, 64);
		gid0[i] = sift_rng_below(&rng, 100);
		parent[i] = i / 4;
	}
	for (int i = 0; i < 1024; i++)
		map[i] = sift_rng_below(&rng, 16);
	for (int i = 0; i < 1000; i++)
	{
		lut2[i] = sift_rng_below(&rng, 2);
		map2[i] = sift_rng_below(&rng, 16);
	}
	for (int i = 0; i < NW; i++)
		rowmask[i] = sift_rng_next(&rng);
	for (int i = 0; i < 64; i++)
		facc[i] = 0, iacc[i] = 0, cnt[i] = 0;

	printf("Vector of %d rows; ns per call, speedup over scalar\n\n| operator | scalar |", N);
	for (int l = 1; l < nlevels; l++)
		printf(" %s | x |", lname[l]);
	printf("\n|---|---|");
	for (int l = 1; l < nlevels; l++)
		printf("---|---|");
	printf("\n");

	ROW("cmp_f64 <", k->cmp_f64(out, f64, N, SCMP_LT, 500));
	ROW("cmp_f64 >", k->cmp_f64(out, f64, N, SCMP_GT, 500));
	ROW("between_f64", k->between_f64(out, f64, N, 250, 750));
	ROW("lut_i32 (1000 entries)", k->lut_i32(out, v2, N, 10, 1000, lut2));
	ROW("gather_bits (4 elements per row)", k->gather_bits(out, rowmask, parent, N));
	ROW("gid_code", k->gid_code(gid0, code, map, N, 4));
	ROW("gid_i32", k->gid_i32(gid0, k32, -5, map2, N, 4));
	ROW("sum_i32 all rows", sink = k->sum_i32(i32, NULL, N));
	ROW("sum_i32 50 %", sink = k->sum_i32(i32, m50, N));
	ROW("sum_f64 all rows", sink = (int64) k->sum_f64(f64, NULL, N));
	ROW("sum_f64 50 %", sink = (int64) k->sum_f64(f64, m50, N));
	{
		int32		a,
					b;
		int64		c,
					d;
		double		e,
					f;

		ROW("minmax_i32 all rows", sink = k->minmax_i32(&a, &b, i32, NULL, N));
		ROW("minmax_i32 50 %", sink = k->minmax_i32(&a, &b, i32, m50, N));
		ROW("minmax_i64 all rows", sink = k->minmax_i64(&c, &d, i64, NULL, N));
		ROW("minmax_i64 50 %", sink = k->minmax_i64(&c, &d, i64, m50, N));
		ROW("minmax_f64 all rows", sink = k->minmax_f64(&e, &f, f64, NULL, N));
		ROW("minmax_f64 50 %", sink = k->minmax_f64(&e, &f, f64, m50, N));
	}
	ROW("count_gid all rows (64 groups)", k->count_gid(cnt, gid, NULL, N));
	ROW("count_gid 50 %", k->count_gid(cnt, gid, m50, N));
	ROW("sum_gid_i32 50 %", k->sum_gid_i32(iacc, gid, i32, m50, N));
	ROW("sum_gid_f64 50 %", k->sum_gid_f64(facc, gid, f64, m50, N));
	{
		uint32		prev = UINT32_MAX;

		ROW("count_changes all rows (10 % change)", sink = k->count_changes(runs, NULL, N, &prev));
		ROW("count_changes 30 %", sink = k->count_changes(runs, mb, N, &prev));
		ROW("count_changes 50 %", sink = k->count_changes(runs, m50, N, &prev));
	}
	return 0;
}
