/*
 * agg/kern_int.h - shared between the per-level aggregation operator files (agg/kern*.c).
 */
#ifndef SIFT_AGGKERN_INT_H
#define SIFT_AGGKERN_INT_H

#include <float.h>
#include <math.h>

#include "agg/kern.h"
#include "col/kern_int.h"

extern const SiftAggKernels sift_aggkern_avx2;
extern const SiftAggKernels sift_aggkern_avx512;
extern const SiftAggKernels sift_aggkern_neon;

#define AGK_NW(n)		(((n) + 63) >> 6)

/* Every slot of the table, in the order of the header */
#define AGK_SLOTS(X) \
	X(cmp_f64) X(cmp2_i32) X(cmp2_i64) X(cmp2_f64) X(lut_i32) X(gather_bits) X(gid_code) X(gid_i32) \
	X(sum_i32) X(absmax_f64) X(absmax_i64) X(minmax_i32) X(minmax_i64) X(minmax_f64) \
	X(lin_i64) X(mulw_i64) \
	X(count_gid) X(sum_gid_i32) X(sum_gid_f64) X(sum_gid_i64)

/* The scalar function of every slot (agg/kern.c): a level that does not implement a slot uses it */
#define AGK_DECLARE(s)	extern __typeof__(*((SiftAggKernels *) 0)->s) agk_##s##_scalar;
AGK_SLOTS(AGK_DECLARE)

/* A NaN result is always this one (the payload of an operand would depend on the instruction used), a zero result is +0 */
static inline double
agk_canon_minmax(double x)
{
	return x != x ? __builtin_nan("") : x == 0 ? 0.0 : x;
}

/* One term of lin_i64 (the scalar specification, and the tails of the vector levels) */
static inline int64
agk_lin_term(int64 x, int32 k)
{
	return k == 1 ? x : k == -1 ? (int64) (0 - (uint64) x) : (int64) (int32) x * (int64) k;
}

#define AGK_UNROLL(n)	SIFT_COLK_UNROLL(n)

#define AGK_LOW(k)		((UINT64CONST(1) << (k)) - 1)

/* The lanes of a group of L (<= 32) rows that exist when r rows are left */
#define AGK_LANES(r, L)	((r) >= (L) ? AGK_LOW(L) : AGK_LOW(r))

/*
 * Writes the predicate bitmap of n rows, L rows per vector step: word w = (OR of (STEP) << j for the steps at rows I =
 * 64 w + j) ^ NEG, bits >= n cleared. STEP is an expression of the row number I giving L bits; TAIL replaces it in the
 * last partial word (a step whose lanes must not touch rows >= n, e.g. a gather). A step may read up to L - 1 rows past n
 * (the padding of the input arrays).
 */
#define AGK_BITS2(out, n, L, NEG, I, STEP, TAIL) \
	do { \
		const int nf_ = (n) >> 6, rem_ = (n) & 63; \
		for (int w_ = 0; w_ < nf_; w_++) \
		{ \
			uint64		b_ = 0; \
			AGK_UNROLL(64) \
			for (int j_ = 0; j_ < 64; j_ += (L)) \
			{ \
				const int I = w_ * 64 + j_; \
				b_ |= (uint64) (uint32) (STEP) << j_; \
			} \
			(out)[w_] = b_ ^ (NEG); \
		} \
		if (rem_) \
		{ \
			uint64		b_ = 0; \
			for (int j_ = 0; j_ < rem_; j_ += (L)) \
			{ \
				const int I = nf_ * 64 + j_; \
				b_ |= (uint64) (uint32) (TAIL) << j_; \
			} \
			(out)[nf_] = (b_ ^ (NEG)) & AGK_LOW(rem_); \
		} \
	} while (0)

#define AGK_BITS(out, n, L, NEG, I, STEP)	AGK_BITS2(out, n, L, NEG, I, STEP, STEP)

#endif							/* SIFT_AGGKERN_INT_H */
