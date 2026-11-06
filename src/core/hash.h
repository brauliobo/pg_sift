/*
 * core/hash.h - the multiply-fold mixers of the hash tables and group keys: the constants are written here once and the steps are inline.
 * The values are stored nowhere but decide the order in which an unordered group output comes out, so a change of a constant or a step changes results
 * (sqlite/tests/hash_test.c pins them). Portable (C library only).
 */
#ifndef SIFT_HASH_H
#define SIFT_HASH_H

#include <stdint.h>

#define SIFT_FIB64	UINT64_C(0x9E3779B97F4A7C15)	/* 2^64 / golden ratio: the good bits are the top ones */
#define SIFT_FOLD64	UINT64_C(0xD6E8FEB86659FD93)	/* the multiplier that follows a xor in a fold */
#define SIFT_FMIX64_A	UINT64_C(0xFF51AFD7ED558CCD)	/* the two multipliers of the 64 bit finalizer of MurmurHash3 */
#define SIFT_FMIX64_B	UINT64_C(0xC4CEB9FE1A85EC53)
#define SIFT_XXP64_2	UINT64_C(0xC2B2AE3D27D4EB4F)	/* the primes of xxHash64 that the hashes of keys with several words take as further multipliers */
#define SIFT_XXP64_3	UINT64_C(0x165667B19E3779F9)
#define SIFT_XXP64_5	UINT64_C(0x27D4EB2F165667C5)

/* Fibonacci hashing: keep the top bits (x >> (64 - bits)) as a slot, or a middle slice as a 32 bit hash */
static inline uint64_t
sift_fib64(uint64_t x)
{
	return x * SIFT_FIB64;
}

/* the next state of a hash that folds values one by one */
static inline uint64_t
sift_fold64(uint64_t h, uint64_t v)
{
	return (h ^ v) * SIFT_FOLD64;
}

/* MurmurHash3 fmix64: every input bit changes every output bit with equal probability */
static inline uint64_t
sift_fmix64(uint64_t x)
{
	x ^= x >> 33;
	x *= SIFT_FMIX64_A;
	x ^= x >> 33;
	x *= SIFT_FMIX64_B;
	return x ^ (x >> 33);
}

#endif
