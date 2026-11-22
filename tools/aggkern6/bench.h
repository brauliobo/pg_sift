/*
 * tools/aggkern6/bench.h - the speed of every slot of the 2048-row operators at each level the CPU has, in ns a row (the best of several
 * runs over one 2048-row batch, hot in the cache), and the speed-up over the scalar table. Included by test.c, which has the tables and
 * the generators:
 *
 *   test bench
 *
 * A row is a row of the batch (the rows a selection vector or a mask leaves out are not counted: a slot on half the rows is timed on the
 * 2048 rows of the batch, as the engine runs it).
 */
#ifndef SIFT_AGGKERN6_BENCH_H
#define SIFT_AGGKERN6_BENCH_H

#include <time.h>

#define BENCH_N		2048
#define BENCH_ITERS	3000
#define BENCH_RUNS	5

static double
bench_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

#define BENCH(label, CALL) \
	do { \
		double ns_[4] = {0, 0, 0, 0}; \
		for (int t = 0; t < ntabs; t++) \
		{ \
			const SiftAggKernelsV6 *k = tabs[t]; \
			double best_ = 1e30; \
			for (int run_ = 0; run_ < BENCH_RUNS; run_++) \
			{ \
				const double t0_ = bench_now(); \
				for (int it_ = 0; it_ < BENCH_ITERS; it_++) \
				{ \
					CALL; \
					__asm__ volatile ("" ::: "memory"); \
				} \
				best_ = Min(best_, bench_now() - t0_); \
			} \
			ns_[t] = best_ / ((double) BENCH_ITERS * BENCH_N); \
		} \
		printf("%-24s", label); \
		for (int t = 0; t < ntabs; t++) \
			printf(" %s %7.3f ns/row (%5.1fx)", names[t], ns_[t], ns_[0] / ns_[t]); \
		printf("\n"); \
	} while (0)

static void
bench_main(void)
{
	const int	n = BENCH_N;
	int32	   *v32 = xalloc(n * 4),
			   *w32 = xalloc(n * 4);
	int64	   *v64 = xalloc(n * 8),
			   *w64 = xalloc(n * 8),
			   *d64 = xalloc(n * 8),
			   *acc = xalloc(65536 * 8),
			   *acc2 = xalloc(65536 * 8);
	SiftI128   *v128 = xalloc(n * 16),
			   *acc128 = xalloc(65536 * 16);
	double	   *vf = xalloc(n * 8),
			   *wf = xalloc(n * 8),
			   *accf = xalloc(65536 * 8),
			   *accf2 = xalloc(65536 * 8);
	uint32	   *vu = xalloc(n * 4),
			   *gid_hot = xalloc(n * 4),
			   *gid_big = xalloc(n * 4),
			   *gid_few = xalloc(n * 4),
			   *gid_run = xalloc(n * 4),
			   *gid_huge = xalloc(n * 4);
	uint64	   *m = xalloc(32 * 8),
			   *m2 = xalloc(32 * 8),
			   *o = xalloc(32 * 8),
			   *h = xalloc(n * 8);
	uint16	   *sel = xalloc((n + SIFT_SEL_SLACK) * 2);
	uint8	   *lut = xalloc(256 + SIFT_AK6_LUT_PAD),
			   *sbuf = xalloc(2 * n);
	SiftStrView *sv = xalloc(n * 16),
			   *svs = xalloc(n * 16);
	int64		set[8];
	int			nsel;
	SiftStrView target;

	for (int i = 0; i < n; i++)
	{
		v32[i] = (int32) rnd();
		w32[i] = rnd() % 4 ? v32[i] : (int32) rnd();
		v64[i] = (int64) rnd();
		w64[i] = rnd() % 4 ? v64[i] : (int64) rnd();
		v128[i].lo = rnd();
		v128[i].hi = (int64) (rnd() >> 20);
		vf[i] = (double) (int64) (rnd() % 2000001) / 64.0;
		wf[i] = rnd() % 4 ? vf[i] : (double) (int64) (rnd() % 2000001) / 64.0;
		vu[i] = 1000 + (uint32) (rnd() % 300);
		gid_hot[i] = rnd() % 64;
		gid_big[i] = rnd() % 4096;
		gid_few[i] = rnd() % 4;
		gid_run[i] = (uint32) i / 40;
		gid_huge[i] = rnd() % 65536;
		h[i] = rnd();
		sbuf[i] = (uint8) ('a' + rnd() % 3);
	}
	for (int i = 0; i < n; i++)
	{
		const uint32 len = i % 5 == 0 ? 20 : 6 + i % 6;

		memset(&sv[i], 0, sizeof(sv[i]));
		sv[i].len = len;
		memcpy(sv[i].prefix, sbuf + i % 64, 4);
		if (len > 12)
			sv[i].u.ptr = (const char *) sbuf + i % 64;
		else
			memcpy(sv[i].u.tail, sbuf + i % 64 + 4, len - 4);
	}
	for (int i = 0; i < n; i++)
	{
		svs[i] = sv[i];
		if (svs[i].len > 12)
		{
			svs[i].len = 12;
			memcpy(svs[i].u.tail, sbuf + i % 64 + 4, 8);
		}
	}
	for (int w = 0; w < 32; w++)
	{
		m[w] = rnd();
		m2[w] = rnd() | rnd();
	}
	for (int i = 0; i < 256 + SIFT_AK6_LUT_PAD; i++)
		lut[i] = rnd() % 2;
	for (int i = 0; i < 8; i++)
		set[i] = v64[i * 100];
	nsel = sift_aggkern6_scalar.mask_to_sel16(sel, m, n);
	target = sv[3];

	printf("%-24s ns a row of a %d row batch, the speed-up over the scalar table in brackets\n", "slot", n);
	BENCH("mask_andn_del", k->mask_andn_del(o, m2, n));
	BENCH("mask_and", k->mask_and(o, m, m2, n));
	BENCH("mask_to_sel16", k->mask_to_sel16(sel, m, n));
	BENCH("key_present_apply", k->key_present_apply(o, m, m2, n, true));
	BENCH("cmp_i32", k->cmp_i32(o, v32, n, SCMP_LT, 0));
	BENCH("cmp_i64", k->cmp_i64(o, v64, n, SCMP_LT, 0));
	BENCH("cmp_f64", k->cmp_f64(o, vf, n, SCMP_LT, 0.0));
	BENCH("cmp_i128", k->cmp_i128(o, v128, n, SCMP_LT, v128[7]));
	BENCH("cmp2_i32", k->cmp2_i32(o, v32, w32, n, SCMP_LT));
	BENCH("cmp2_i64", k->cmp2_i64(o, v64, w64, n, SCMP_LT));
	BENCH("cmp2_f64", k->cmp2_f64(o, vf, wf, n, SCMP_LT));
	BENCH("lut_u32", k->lut_u32(o, vu, n, 1000, 256, lut));
	BENCH("in_set_i64 (8)", k->in_set_i64(o, v64, n, set, 8));
	BENCH("cmp_strview_eq", k->cmp_strview_eq(o, sv, n, target));
	BENCH("stream_run_ne_i64", k->stream_run_ne_i64(o, v64, m2, n));
	BENCH("hash_i32", k->hash_i32(h, v32, NULL, n, true));
	BENCH("hash_i32 sel", k->hash_i32(h, v32, sel, nsel, true));
	BENCH("hash_i64", k->hash_i64(h, v64, NULL, n, true));
	BENCH("hash_i64 sel", k->hash_i64(h, v64, sel, nsel, true));
	BENCH("hash_i128", k->hash_i128(h, v128, NULL, n, true));
	BENCH("hash_i128 sel", k->hash_i128(h, v128, sel, nsel, true));
	BENCH("hash_strview mixed", k->hash_strview(h, sv + 1, NULL, n - 1, true));
	BENCH("hash_strview short", k->hash_strview(h, svs, NULL, n, true));
	BENCH("sum_i32 mask", k->sum_i32(v32, m, n));
	BENCH("sum_i64_checked mask", k->sum_i64_checked(v64, m, n, &d64[0]));
	BENCH("sum_i64_to_i128 mask", k->sum_i64_to_i128(v64, m, n, (SiftI128) {0, 0}));
	BENCH("sum_i128 mask", k->sum_i128(v128, m, n, (SiftI128) {0, 0}));
	BENCH("sum_f64_fast mask", k->sum_f64_fast(vf, m, n, 0.0));
	BENCH("minmax_i32 mask", k->minmax_i32((int32 *) &d64[0], (int32 *) &d64[1], v32, m, n));
	BENCH("minmax_i64 mask", k->minmax_i64(&d64[0], &d64[1], v64, m, n));
	BENCH("minmax_i128 mask", k->minmax_i128((SiftI128 *) &d64[0], (SiftI128 *) &d64[2], v128, m, n));
	BENCH("minmax_f64 mask", k->minmax_f64((double *) &d64[0], (double *) &d64[1], vf, m, n));
	BENCH("count_gid 4", k->count_gid(acc, gid_few, NULL, n));
	BENCH("count_gid runs of 40", k->count_gid(acc, gid_run, NULL, n));
	BENCH("count_gid 65536", k->count_gid(acc, gid_huge, NULL, n));
	BENCH("sum_gid_i64 4", k->sum_gid_i64(acc, gid_few, v64, NULL, n));
	BENCH("sum_gid_i64 runs of 40", k->sum_gid_i64(acc, gid_run, v64, NULL, n));
	BENCH("sum_gid_i64 65536", k->sum_gid_i64(acc, gid_huge, v64, NULL, n));
	BENCH("sum_gid_f64 4", k->sum_gid_f64_fast(accf, gid_few, vf, NULL, n));
	BENCH("sum_gid_f64 runs of 40", k->sum_gid_f64_fast(accf, gid_run, vf, NULL, n));
	BENCH("count_gid hot 64", k->count_gid(acc, gid_hot, NULL, n));
	BENCH("count_gid 4096", k->count_gid(acc, gid_big, NULL, n));
	BENCH("count_gid 4096 sel", k->count_gid(acc, gid_big, sel, nsel));
	BENCH("sum_gid_i32 hot 64", k->sum_gid_i32(acc, gid_hot, v32, NULL, n));
	BENCH("sum_gid_i64 hot 64", k->sum_gid_i64(acc, gid_hot, v64, NULL, n));
	BENCH("sum_gid_i64 4096", k->sum_gid_i64(acc, gid_big, v64, NULL, n));
	BENCH("sum_gid_i64 4096 sel", k->sum_gid_i64(acc, gid_big, v64, sel, nsel));
	BENCH("sum_gid_i64_i128 4096", k->sum_gid_i64_to_i128(acc128, gid_big, v64, NULL, n));
	BENCH("sum_gid_i128 4096", k->sum_gid_i128(acc128, gid_big, v128, NULL, n));
	BENCH("sum_gid_f64 4096", k->sum_gid_f64_fast(accf, gid_big, vf, NULL, n));
	BENCH("minmax_gid_i64 hot 64", k->minmax_gid_i64(acc, acc2, gid_hot, v64, NULL, n));
	BENCH("minmax_gid_i64 4096", k->minmax_gid_i64(acc, acc2, gid_big, v64, NULL, n));
	BENCH("minmax_gid_f64 4096", k->minmax_gid_f64(accf, accf2, gid_big, vf, NULL, n));
	BENCH("lin_i64", k->lin_i64(d64, v64, 3, w64, -5, n));
	BENCH("mulw_i64", k->mulw_i64(d64, v64, w64, n));
}

#endif							/* SIFT_AGGKERN6_BENCH_H */
