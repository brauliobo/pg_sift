/*
 * Aggregation operator throughput per level: ns per call over one vector of 4096 rows (and the speedup over scalar). Single thread
 * pinned to KT_CPU (default 2); run under flock /tmp/sift-perf.lock (make bench). Output is a markdown table.
 *
 * Inputs: random data, 50 % selection masks unless the row says otherwise (dens = fraction of rows selected).
 */
#define _GNU_SOURCE
#include "harness.h"

#include <sched.h>
#include <time.h>

#include "sift_aggkern_cases.h"

extern int	alt_mask_sel_compressb(uint32 *sel, const uint64 *mask, int nw);
extern int	alt_mask_sel_avx2_sparse(uint32 *sel, const uint64 *mask, int nw, int thr);
extern int	alt_mask_sel_avx512_sparse(uint32 *sel, const uint64 *mask, int nw, int thr);
extern double alt_sum_f64_a(const double *v, const uint64 *mask, int n);
extern double alt_sum_f64_b(const double *v, const uint64 *mask, int n);
extern double alt_sum_f64_c(const double *v, const uint64 *mask, int n);
extern double alt_sum_f64_plain(const double *v, const uint64 *mask, int n);

#define N			SIFT_AGG_VEC
#define NW			SIFT_AGG_VECW

static double
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

/* Best ns per evaluation of expr: batches of >= 50 us, minimum over 200 batches, AK_BATCHES to change (the host runs unrelated load) */
#define BENCH(expr) ({ \
	long		reps_ = 1; \
	double		best_ = 1e300; \
	for (;;) { \
		double t0_ = now_ns(); \
		for (long i_ = 0; i_ < reps_; i_++) { expr; __asm__ volatile ("" ::: "memory"); } \
		if (now_ns() - t0_ > 5e4) break; \
		reps_ *= 2; \
	} \
	for (int r_ = 0; r_ < nbatch; r_++) { \
		double t0_ = now_ns(), d_; \
		for (long i_ = 0; i_ < reps_; i_++) { expr; __asm__ volatile ("" ::: "memory"); } \
		d_ = (now_ns() - t0_) / reps_; \
		best_ = d_ < best_ ? d_ : best_; \
	} \
	best_; })

static AgkRng rng;
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
		p[i] = (uint8) agk_next(&rng);
	return p;
}

static uint64 *
make_mask(double dens)
{
	uint64	   *m = arr(NW * 8);

	memset(m, 0, NW * 8);
	for (int i = 0; i < N; i++)
		if (agk_unit(&rng) < dens)
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
			   *parent,
			   *sel,
			   *ssel,
			   *dst32;
	uint64	   *dst64,
			   *out,
			   *w2,
			   *rowmask,
			   *m50,
			   *m1,
			   *m10,
			   *m100,
			   *mb;
	uint8	   *lut,
			   *lut2,
			   *bytes;
	int64	   *cnt,
			   *iacc;
	double	   *facc;
	int			nsel;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
	ak_cpu_init();
	nbatch = getenv("AK_BATCHES") ? atoi(getenv("AK_BATCHES")) : nbatch;
	agk_seed(&rng, 42);
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
	sel = arr(N * 4 + 64);
	ssel = arr(N * 4 + 64);
	dst32 = arr(N * 4);
	dst64 = arr(N * 8);
	out = arr(NW * 8);
	w2 = arr(NW * 8);
	rowmask = arr(NW * 8);
	m50 = make_mask(0.5);
	m1 = make_mask(0.01);
	m10 = make_mask(0.1);
	m100 = make_mask(1.0);
	mb = make_mask(0.3);
	lut = arr(1024);
	lut2 = arr(1000);
	bytes = arr(N);
	cnt = arr(64 * 8);
	iacc = arr(64 * 8);
	facc = arr(64 * 8);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (l == 0 || sift_aggkern_for(l) != levels[nlevels - 1])
		{
			levels[nlevels] = sift_aggkern_for(l);
			lname[nlevels++] = ak_level_name[l];
		}

	for (int i = 0; i < N; i++)
	{
		i32[i] = agk_below(&rng, 1000000);
		i64[i] = (int64) (agk_next(&rng) >> 24);
		f64[i] = agk_unit(&rng) * 1000;
		code[i] = agk_below(&rng, 1024);
		runs[i] = i > 0 && agk_below(&rng, 10) ? runs[i - 1] : agk_below(&rng, 1024);
		v2[i] = 7 + agk_below(&rng, 1200);
		k32[i] = -5 + agk_below(&rng, 1000);
		gid[i] = agk_below(&rng, 64);
		gid0[i] = agk_below(&rng, 100);
		parent[i] = i / 4;
		bytes[i] = agk_below(&rng, 2);
	}
	for (int i = 0; i < 1024; i++)
	{
		lut[i] = agk_below(&rng, 2);
		map[i] = agk_below(&rng, 16);
	}
	for (int i = 0; i < 1000; i++)
	{
		lut2[i] = agk_below(&rng, 2);
		map2[i] = agk_below(&rng, 16);
	}
	for (int i = 0; i < NW; i++)
		rowmask[i] = agk_next(&rng);
	for (int i = 0; i < 64; i++)
		facc[i] = 0, iacc[i] = 0, cnt[i] = 0;
	nsel = sift_aggkern_scalar.mask_sel(ssel, m50, NW);

	printf("Vector of %d rows; ns per call, speedup over scalar\n\n| operator | scalar |", N);
	for (int l = 1; l < nlevels; l++)
		printf(" %s | x |", lname[l]);
	printf("\n|---|---|");
	for (int l = 1; l < nlevels; l++)
		printf("---|---|");
	printf("\n");

	ROW("cmp_i32 <", k->cmp_i32(out, i32, N, SCMP_LT, 500000));
	ROW("cmp_i32 ==", k->cmp_i32(out, i32, N, SCMP_EQ, 500000));
	ROW("cmp_i64 <", k->cmp_i64(out, i64, N, SCMP_LT, INT64CONST(1) << 39));
	ROW("cmp_f64 <", k->cmp_f64(out, f64, N, SCMP_LT, 500));
	ROW("cmp_f64 >", k->cmp_f64(out, f64, N, SCMP_GT, 500));
	ROW("between_i32", k->between_i32(out, i32, N, 250000, 750000));
	ROW("between_i64", k->between_i64(out, i64, N, INT64CONST(1) << 38, INT64CONST(3) << 38));
	ROW("between_f64", k->between_f64(out, f64, N, 250, 750));
	ROW("lut_code (1024 codes)", k->lut_code(out, code, N, lut));
	ROW("lut_i32 (1000 entries)", k->lut_i32(out, v2, N, 10, 1000, lut2));
	ROW("bytes_mask", k->bytes_mask(out, bytes, N));
	ROW("and_w", k->and_w(w2, m50, NW));
	ROW("or_w", k->or_w(w2, m50, NW));
	ROW("andnot_w", k->andnot_w(w2, m50, NW));
	ROW("popcount_w", sink = k->popcount_w(m50, NW));
	ROW("gather_bits (4 elements per row)", k->gather_bits(out, rowmask, parent, N));
	ROW("mask_sel 1 %", sink = k->mask_sel(sel, m1, NW));
	ROW("mask_sel 10 %", sink = k->mask_sel(sel, m10, NW));
	ROW("mask_sel 50 %", sink = k->mask_sel(sel, m50, NW));
	ROW("mask_sel 100 %", sink = k->mask_sel(sel, m100, NW));
	ROW("gather_u32 (2048 of 4096)", k->gather_u32(dst32, code, ssel, nsel));
	ROW("gather_u64 (2048 of 4096)", k->gather_u64(dst64, (const uint64 *) i64, ssel, nsel));
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
	printf("\nCandidate designs (ns per call):\n\n| mask_sel design | 1 %% | 10 %% | 50 %% | 100 %% |\n|---|---|---|---|---|\n");
	{
		uint64	   *ms[] = {m1, m10, m50, m100};
		static const int thr[] = {0, 2, 4, 8, 16};

		if (sift_aggkern_for(SIFT_SIMD_AVX2) != &sift_aggkern_scalar)
			for (int t = 0; t < lengthof(thr); t++)
			{
				printf("| AVX2, words with <= %d bits scalar |", thr[t]);
				for (int d = 0; d < 4; d++)
					printf(" %.0f |", BENCH(sink = alt_mask_sel_avx2_sparse(sel, ms[d], NW, thr[t])));
				printf("\n");
			}
		if (sift_aggkern_for(SIFT_SIMD_AVX512) != sift_aggkern_for(SIFT_SIMD_AVX2))
		{
			for (int t = 0; t < lengthof(thr); t++)
			{
				printf("| AVX-512 VPCOMPRESSD, words with <= %d bits scalar |", thr[t]);
				for (int d = 0; d < 4; d++)
					printf(" %.0f |", BENCH(sink = alt_mask_sel_avx512_sparse(sel, ms[d], NW, thr[t])));
				printf("\n");
			}
			printf("| AVX-512 VPCOMPRESSB |");
			for (int d = 0; d < 4; d++)
				printf(" %.0f |", BENCH(sink = alt_mask_sel_compressb(sel, ms[d], NW)));
			printf("\n");
		}
	}
	if (sift_aggkern_for(SIFT_SIMD_AVX512) != sift_aggkern_for(SIFT_SIMD_AVX2))
		printf("\n| sum_f64, AVX-512 selection | 50 %% | all rows |\n|---|---|---|\n| zeroing masked load | %.0f | |\n"
			   "| load + zeroing move | %.0f | |\n| load + AND with a lane table | %.0f | |\n| plain loads | | %.0f |\n",
			   BENCH(sink = (int64) alt_sum_f64_a(f64, m50, N)), BENCH(sink = (int64) alt_sum_f64_b(f64, m50, N)),
			   BENCH(sink = (int64) alt_sum_f64_c(f64, m50, N)), BENCH(sink = (int64) alt_sum_f64_plain(f64, NULL, N)));
	return 0;
}
