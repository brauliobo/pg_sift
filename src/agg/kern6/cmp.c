/*
 * agg/kern6/cmp.c - the scalar predicates of the 2048-row engine (the specification every SIMD level matches bit for bit): comparisons with a
 * constant and between two arrays, a lookup table, a set, a string, and the run boundaries of a sorted column. Each writes ceil(n / 64) whole
 * words, the bits >= n zero. Baseline x86-64 code.
 */
#include "agg/kern6/int.h"

/* The words of n bits, bit i set when HOLDS(i) */
#define WORDS(out, n, i, HOLDS) \
	do { \
		const int nw_ = AK6_NW(n); \
		for (int w_ = 0; w_ < nw_; w_++) \
		{ \
			const int end_ = Min(w_ * 64 + 64, (n)); \
			uint64 word_ = 0; \
			for (int i = w_ * 64; i < end_; i++) \
				word_ |= (uint64) (HOLDS) << (i & 63); \
			(out)[w_] = word_; \
		} \
	} while (0)

#define THREE(a, b)		((a) < (b) ? -1 : (a) > (b))

void
ak6_cmp_i32_scalar(uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c)
{
	WORDS(out, n, i, ak6_op_holds(op, THREE(v[i], c)));
}

void
ak6_cmp_i64_scalar(uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c)
{
	WORDS(out, n, i, ak6_op_holds(op, THREE(v[i], c)));
}

void
ak6_cmp_f64_scalar(uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c)
{
	WORDS(out, n, i, ak6_op_holds(op, ak6_f8_cmp(v[i], c)));
}

void
ak6_cmp_i128_scalar(uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c)
{
	WORDS(out, n, i, ak6_op_holds(op, ak6_i128_cmp(v[i], c)));
}

void
ak6_cmp2_i32_scalar(uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op)
{
	WORDS(out, n, i, ak6_op_holds(op, THREE(a[i], b[i])));
}

void
ak6_cmp2_i64_scalar(uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op)
{
	WORDS(out, n, i, ak6_op_holds(op, THREE(a[i], b[i])));
}

void
ak6_cmp2_f64_scalar(uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op)
{
	WORDS(out, n, i, ak6_op_holds(op, ak6_f8_cmp(a[i], b[i])));
}

void
ak6_lut_u32_scalar(uint64 *out, const uint32 *v, uint16 n, uint32 base, uint32 nlut, const uint8 *lut)
{
	WORDS(out, n, i, (uint32) (v[i] - base) < nlut && lut[v[i] - base] != 0);
}

static inline bool
in_set(int64 x, const int64 *set, uint8 nset)
{
	for (int k = 0; k < nset; k++)
		if (x == set[k])
			return true;
	return false;
}

void
ak6_in_set_i64_scalar(uint64 *out, const int64 *v, uint16 n, const int64 *set, uint8 nset)
{
	WORDS(out, n, i, in_set(v[i], set, nset));
}

void
ak6_cmp_strview_eq_scalar(uint64 *out, const SiftStrView *v, uint16 n, SiftStrView target)
{
	WORDS(out, n, i, ak6_sv_eq(&v[i], &target));
}

void
ak6_stream_run_ne_i64_scalar(uint64 *out, const int64 *v, const uint64 *valid, uint16 n)
{
	WORDS(out, n, i,
		  i == 0 || (valid ? AK6_ROWBIT(valid, i) != AK6_ROWBIT(valid, i - 1) || (AK6_ROWBIT(valid, i) && v[i] != v[i - 1]) : v[i] != v[i - 1]));
}
