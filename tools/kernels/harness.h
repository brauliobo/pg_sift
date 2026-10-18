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

/* libc, not libpgport */
#undef printf
#undef fprintf
#undef qsort

typedef struct Rng
{
	uint64		s[4];
} Rng;

static inline uint64
rng_split(uint64 *x)
{
	uint64		z = (*x += UINT64CONST(0x9E3779B97F4A7C15));

	z = (z ^ (z >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return z ^ (z >> 31);
}

static inline void
rng_seed(Rng *r, uint64 seed)
{
	for (int i = 0; i < 4; i++)
		r->s[i] = rng_split(&seed);
}

/* xoshiro256** */
static inline uint64
rng_next(Rng *r)
{
	uint64	   *s = r->s,
				x = s[1] * 5,
				t = s[1] << 17;

	x = ((x << 7) | (x >> 57)) * 9;
	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = (s[3] << 45) | (s[3] >> 19);
	return x;
}

static inline uint32
rng_below(Rng *r, uint32 n)
{
	return (uint32) (((rng_next(r) >> 32) * n) >> 32);
}

static inline double
rng_unit(Rng *r)
{
	return (rng_next(r) >> 11) * 0x1p-53;
}

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
