/* Shared by the standalone kernel test and benchmark (no backend linked). */
#ifndef KT_HARNESS_H
#define KT_HARNESS_H

#include "postgres.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bm25/kernels_int.h"
#include "core/testrng.h"

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
 * Best ns per evaluation of expr: batches of >= window_ns, minimum over nbatch batches (the host runs unrelated load; the minimum
 * filters preemption); fence runs after every evaluation.
 */
#define BENCH_EX(expr, window_ns, nbatch, fence) ({ \
	long		reps_ = 1; \
	double		best_ = 1e300; \
	for (;;) { \
		double t0_ = now_ns(); \
		for (long i_ = 0; i_ < reps_; i_++) { expr; fence; } \
		if (now_ns() - t0_ > (window_ns)) break; \
		reps_ *= 2; \
	} \
	for (int r_ = 0; r_ < (nbatch); r_++) { \
		double t0_ = now_ns(), d_; \
		for (long i_ = 0; i_ < reps_; i_++) { expr; fence; } \
		d_ = (now_ns() - t0_) / reps_; \
		best_ = d_ < best_ ? d_ : best_; \
	} \
	best_; })
#define BENCH(expr) BENCH_EX(expr, 2e4, 300, (void) 0)

/* One line of a benchmark table: lanes per call, bytes touched per lane (bench.c) */
extern void row(const char *kernel, const char *scen, const char *lvl, double ns, double lanes, double bytes);
extern void bench_col(void);
extern void bench_shred(void);

extern void kt_cpu_init(void);
extern const char *const kt_level_name[];

#endif
