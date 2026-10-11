/* Shared by the standalone kernel test and benchmark (no backend linked). */
#ifndef KT_HARNESS_H
#define KT_HARNESS_H

#include "postgres.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

extern void kt_cpu_init(void);
extern const char *const kt_level_name[];

#endif
