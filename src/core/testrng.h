/*
 * core/testrng.h - the deterministic generators of the tests and the standalone harnesses: SplitMix64 (a stateless mix and a stream) and
 * xoshiro256** seeded from it. Test code only; C library only.
 */
#ifndef SIFT_TESTRNG_H
#define SIFT_TESTRNG_H

#include <stdint.h>

#define SIFT_SPLITMIX_GAMMA	UINT64_C(0x9E3779B97F4A7C15)

/* The output for state x (a hash of x + 1 gamma) */
static inline uint64_t
sift_mix64(uint64_t x)
{
	x += SIFT_SPLITMIX_GAMMA;
	x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
	x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
	return x ^ (x >> 31);
}

/* The next output of the stream whose state is *s */
static inline uint64_t
sift_splitmix_next(uint64_t *s)
{
	uint64_t	z = sift_mix64(*s);

	*s += SIFT_SPLITMIX_GAMMA;
	return z;
}

typedef struct SiftRng
{
	uint64_t	s[4];
} SiftRng;

static inline void
sift_rng_seed(SiftRng *r, uint64_t seed)
{
	for (int i = 0; i < 4; i++)
		r->s[i] = sift_splitmix_next(&seed);
}

/* xoshiro256** */
static inline uint64_t
sift_rng_next(SiftRng *r)
{
	uint64_t   *s = r->s,
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

static inline uint32_t
sift_rng_below(SiftRng *r, uint32_t n)
{
	return (uint32_t) (((sift_rng_next(r) >> 32) * n) >> 32);
}

static inline double
sift_rng_unit(SiftRng *r)
{
	return (sift_rng_next(r) >> 11) * 0x1p-53;
}

#endif							/* SIFT_TESTRNG_H */
