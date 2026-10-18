/* Shared by the standalone kernel test and benchmark (no backend linked). */
#ifndef KT_HARNESS_H
#define KT_HARNESS_H

#include "postgres.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sift_kernels_int.h"
#include "sift_testrng.h"

/* libc, not libpgport */
#undef printf
#undef fprintf
#undef qsort

static inline void *
kt_alloc(size_t n)
{
	void	   *p = aligned_alloc(SIFT_SIMD_ALIGN, (n + SIFT_SIMD_ALIGN - 1) & ~(size_t) (SIFT_SIMD_ALIGN - 1));

	if (!p)
		abort();
	return p;
}

static inline double
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

/*
 * Best ns per evaluation of expr: batches of >= 20 us, minimum over 300
 * batches (the host runs unrelated load; the minimum filters preemption).
 */
#define BENCH(expr) ({ \
	long		reps_ = 1; \
	double		best_ = 1e300; \
	for (;;) { \
		double t0_ = now_ns(); \
		for (long i_ = 0; i_ < reps_; i_++) { expr; } \
		if (now_ns() - t0_ > 2e4) break; \
		reps_ *= 2; \
	} \
	for (int r_ = 0; r_ < 300; r_++) { \
		double t0_ = now_ns(), d_; \
		for (long i_ = 0; i_ < reps_; i_++) { expr; } \
		d_ = (now_ns() - t0_) / reps_; \
		best_ = d_ < best_ ? d_ : best_; \
	} \
	best_; })

/* One line of a benchmark table: lanes per call, bytes touched per lane (bench.c) */
extern void row(const char *kernel, const char *scen, const char *lvl, double ns, double lanes, double bytes);
extern void bench_col(void);
extern void bench_shred(void);

extern void kt_cpu_init(void);
extern const char *const kt_level_name[];

#endif
