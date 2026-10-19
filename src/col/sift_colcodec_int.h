/*
 * sift_colcodec_int.h - shared between sift_colcodec.c and the per-level kernel files (the vertical bit-packing layout and the tables).
 */
#ifndef SIFT_COLCODEC_INT_H
#define SIFT_COLCODEC_INT_H

#include "sift_colcodec.h"

extern const SiftCcKernels sift_cc_kernels_scalar;
extern const SiftCcKernels sift_cc_kernels_avx2;
extern const SiftCcKernels sift_cc_kernels_avx512;

/* Rows of 32 values for n values */
#define SIFT_CC_ROWS(n)		(((uint64) (n) + 31) >> 5)

/* u32 words of n values bit-packed at width W in the vertical layout: full blocks of 32 rows take 32 * W, a tail of R rows 32 * ceil(R * W / 32) */
static inline uint64
sift_cc_words(uint32 n, uint32 W)
{
	uint64		rows = SIFT_CC_ROWS(n),
				w = (rows / 32) * 32 * W;

	if (rows % 32)
		w += 32 * (((rows % 32) * W + 31) / 32);
	return w;
}

static inline uint32
sift_cc_bits_of(uint64 x)
{
	uint32		w = 0;

	for (; x; x >>= 1)
		w++;
	return w;
}

static inline uint32
sift_cc_ld32(const uint32 *p)
{
	uint32		x;

	memcpy(&x, p, sizeof(x));
	return x;
}

static inline uint64
sift_cc_ld64(const void *p)
{
	uint64		x;

	memcpy(&x, p, sizeof(x));
	return x;
}

/* The decimal exponents of the ALP node: 10^e exactly as double and float (e <= 18, resp. 10) */
extern const double sift_cc_p10[19];
extern const float sift_cc_p10f[11];
#define SIFT_CC_ALP_EMAX_F8	18
#define SIFT_CC_ALP_EMAX_F4	10

#endif							/* SIFT_COLCODEC_INT_H */
