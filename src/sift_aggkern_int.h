/*
 * sift_aggkern_int.h - shared between the per-level aggregation operator files (sift_aggkern*.c).
 */
#ifndef SIFT_AGGKERN_INT_H
#define SIFT_AGGKERN_INT_H

#include <math.h>

#include "sift_aggkern.h"
#include "sift_popcount.h"

extern const SiftAggKernels sift_aggkern_avx2;
extern const SiftAggKernels sift_aggkern_avx512;

#define AGK_NW(n)		(((n) + 63) >> 6)

/* Every slot of the table, in the order of the header */
#define AGK_SLOTS(X) \
	X(cmp_i32) X(cmp_i64) X(cmp_f64) X(between_i32) X(between_i64) X(between_f64) \
	X(lut_code) X(lut_i32) X(bytes_mask) \
	X(and_w) X(or_w) X(andnot_w) X(popcount_w) X(gather_bits) \
	X(mask_sel) X(gather_u32) X(gather_u64) X(gid_code) X(gid_i32) \
	X(sum_i32) X(sum_f64) X(minmax_i32) X(minmax_i64) X(minmax_f64) \
	X(count_gid) X(sum_gid_i32) X(sum_gid_f64) X(count_changes)

/* The scalar function of every slot (sift_aggkern.c): a level that does not implement a slot uses it */
#define AGK_DECLARE(s)	extern __typeof__(*((SiftAggKernels *) 0)->s) agk_##s##_scalar;
AGK_SLOTS(AGK_DECLARE)

/* A NaN result is always this one (the payload of an operand would depend on the instruction used), a zero result is +0 */
static inline double
agk_canon_nan(double x)
{
	return x != x ? __builtin_nan("") : x;
}

static inline double
agk_canon_minmax(double x)
{
	return x != x ? __builtin_nan("") : x == 0 ? 0.0 : x;
}

/* Fully unrolled loop of a constant trip count (a vector step per iteration), whichever compiler builds the file */
#if defined(__clang__)
#define AGK_UNROLL(n)	_Pragma("clang loop unroll(full)")
#else
#define AGK_UNROLL(n)	AGK_PRAGMA(GCC unroll n)
#define AGK_PRAGMA(x)	_Pragma(#x)
#endif

/* mask_sel: a word with at most this many bits is walked bit by bit, the vector expansion costs more */
#define AGK_SEL_SPARSE	8

#define AGK_LOW(k)		((UINT64CONST(1) << (k)) - 1)

/* The lanes of a group of L (<= 32) rows that exist when r rows are left */
#define AGK_LANES(r, L)	((r) >= (L) ? AGK_LOW(L) : AGK_LOW(r))

/* The set bits of a mask byte, or of all rows when mask is NULL (the last group has the rows left) */
#define AGK_SELBITS(mask, i, n, L) \
	((mask) ? (uint32) (((mask)[(i) >> 6] >> ((i) & 63)) & AGK_LOW(L)) : (uint32) AGK_LANES((n) - (i), L))

#define AGK_BY_OP(op, FN, ...) \
	switch (op) \
	{ \
		case SCMP_EQ: FN(SCMP_EQ, __VA_ARGS__); break; \
		case SCMP_NE: FN(SCMP_NE, __VA_ARGS__); break; \
		case SCMP_LT: FN(SCMP_LT, __VA_ARGS__); break; \
		case SCMP_LE: FN(SCMP_LE, __VA_ARGS__); break; \
		case SCMP_GT: FN(SCMP_GT, __VA_ARGS__); break; \
		default: FN(SCMP_GE, __VA_ARGS__); break; \
	}

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
