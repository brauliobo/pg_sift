/* Bit counting without libgcc, pure C (included by the codec and the kernels). */
#ifndef SIFT_POPCOUNT_H
#define SIFT_POPCOUNT_H

#include <stdint.h>

/*
 * Bits set in baseline x86-64 code: that code would call libgcc's __popcountdi2,
 * which a distribution may build with POPCNT. Code with the popcnt target
 * (the AVX2 and AVX-512 files) keeps __builtin_popcountll.
 */
static inline uint32_t sift_pc64(uint64_t x)
{
	x -= (x >> 1) & 0x5555555555555555ULL;
	x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
	x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
	return (uint32_t)((x * 0x0101010101010101ULL) >> 56);
}

#endif							/* SIFT_POPCOUNT_H */
