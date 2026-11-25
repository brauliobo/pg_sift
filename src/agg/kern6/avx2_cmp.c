/*
 * agg/kern6/avx2_cmp.c - the AVX2 predicates of the 2048-row engine (agg/kern6/cmp.c is the specification): a vector compare gives the
 * less-than and equal masks of 4 or 8 rows, the movemask turns them into bits, and the operator picks its bits from them (ak6_cmp_sel), so
 * one body serves the six operators; the operator is a constant of each copy of the body. Doubles take the float8 order from two masks
 * (the NaNs of each side). A 128-bit value is compared as the pair (signed high, unsigned low) with the result of the high half
 * shuffled over the low one. The rows that do not fill a vector are the scalar rows.
 */
#include "core/host.h"

#include "agg/kern6/avx2.h"

#if defined(__x86_64__)

/* The words of n bits: BITS the bits of the W rows from row j, ROW the bit of the row j of the rows that do not fill a vector */
#define PRED_BITS(out, n, W, BITS, ROW) \
	do { \
		const int nw_ = AK6_NW(n); \
		for (int w_ = 0; w_ < nw_; w_++) \
		{ \
			const int base_ = w_ * 64, end_ = Min(base_ + 64, (int) (n)); \
			uint64 word_ = 0; \
			int j = base_; \
			for (; j + (W) <= end_; j += (W)) \
				word_ |= (uint64) (BITS) << (j - base_); \
			for (; j < end_; j++) \
				word_ |= (uint64) (ROW) << (j - base_); \
			(out)[w_] = word_; \
		} \
	} while (0)

#define THREE(a, b)		((a) < (b) ? -1 : (a) > (b))
#define MS32(x)			((uint32) _mm256_movemask_ps(_mm256_castsi256_ps(x)))
#define MS64(x)			((uint32) _mm256_movemask_pd(_mm256_castsi256_pd(x)))
#define MSPD(x)			((uint32) _mm256_movemask_pd(x))

/* The same body for each operator, so that the operator is a constant of it */
#define FOR_OPS(CASE)	CASE(SCMP_EQ) CASE(SCMP_NE) CASE(SCMP_LT) CASE(SCMP_LE) CASE(SCMP_GT) CASE(SCMP_GE)

/* ---- int32, int64, float8: against a constant and between arrays ---- */

K2 uint64
bits_i32(SiftCmpOp op, __m256i x, __m256i y)
{
	return ak6_cmp_sel(op, MS32(_mm256_cmpgt_epi32(y, x)), MS32(_mm256_cmpeq_epi32(x, y)), 0xFF);
}

K2 uint64
bits_i64(SiftCmpOp op, __m256i x, __m256i y)
{
	return ak6_cmp_sel(op, MS64(_mm256_cmpgt_epi64(y, x)), MS64(_mm256_cmpeq_epi64(x, y)), 0xF);
}

/* yn: the lanes of y that are NaN */
K2 uint64
bits_f64(SiftCmpOp op, __m256d x, __m256d y, __m256d yn)
{
	const __m256d xn = _mm256_cmp_pd(x, x, _CMP_UNORD_Q),
				lt = _mm256_or_pd(_mm256_cmp_pd(x, y, _CMP_LT_OQ), _mm256_andnot_pd(xn, yn)),
				eq = _mm256_or_pd(_mm256_cmp_pd(x, y, _CMP_EQ_OQ), _mm256_and_pd(xn, yn));

	return ak6_cmp_sel(op, MSPD(lt), MSPD(eq), 0xF);
}

K2 void
cmp_i32_k(uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c)
{
	const __m256i cv = SET32(c);

	PRED_BITS(out, n, 8, bits_i32(op, LD(v + j), cv), ak6_op_holds(op, THREE(v[j], c)));
}

K2 void
cmp_i64_k(uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c)
{
	const __m256i cv = SET64(c);

	PRED_BITS(out, n, 4, bits_i64(op, LD(v + j), cv), ak6_op_holds(op, THREE(v[j], c)));
}

K2 void
cmp_f64_k(uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c)
{
	const __m256d cv = _mm256_set1_pd(c),
				cn = _mm256_cmp_pd(cv, cv, _CMP_UNORD_Q);

	PRED_BITS(out, n, 4, bits_f64(op, LDPD(v + j), cv, cn), ak6_op_holds(op, ak6_f8_cmp(v[j], c)));
}

K2 void
cmp2_i32_k(uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op)
{
	PRED_BITS(out, n, 8, bits_i32(op, LD(a + j), LD(b + j)), ak6_op_holds(op, THREE(a[j], b[j])));
}

K2 void
cmp2_i64_k(uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op)
{
	PRED_BITS(out, n, 4, bits_i64(op, LD(a + j), LD(b + j)), ak6_op_holds(op, THREE(a[j], b[j])));
}

K2 void
cmp2_f64_k(uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op)
{
	PRED_BITS(out, n, 4,
			  bits_f64(op, LDPD(a + j), LDPD(b + j), _mm256_cmp_pd(LDPD(b + j), LDPD(b + j), _CMP_UNORD_Q)),
			  ak6_op_holds(op, ak6_f8_cmp(a[j], b[j])));
}

X2 void
ak6_cmp_i32_avx2(uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_i32_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X2 void
ak6_cmp_i64_avx2(uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_i64_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X2 void
ak6_cmp_f64_avx2(uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_f64_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X2 void
ak6_cmp2_i32_avx2(uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op)
{
	switch (op)
	{
#define CASE(o) case o: cmp2_i32_k(out, a, b, n, o); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X2 void
ak6_cmp2_i64_avx2(uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op)
{
	switch (op)
	{
#define CASE(o) case o: cmp2_i64_k(out, a, b, n, o); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X2 void
ak6_cmp2_f64_avx2(uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op)
{
	switch (op)
	{
#define CASE(o) case o: cmp2_f64_k(out, a, b, n, o); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

/* ---- int128: two values a vector, the lanes (low, high, low, high) ---- */

/* The bits of the two values: the order of a pair is that of its high half, then of its low half as unsigned (the sign bit flipped, to use the signed compare) */
K2 uint64
bits_i128(SiftCmpOp op, __m256i x, __m256i c)
{
	const __m256i flip = _mm256_set_epi64x(0, SIGN64, 0, SIGN64),
				xa = _mm256_xor_si256(x, flip),
				ca = _mm256_xor_si256(c, flip),
				lt = _mm256_cmpgt_epi64(ca, xa),
				eq = _mm256_cmpeq_epi64(xa, ca),
				hi_lt = _mm256_shuffle_epi32(lt, _MM_SHUFFLE(3, 2, 3, 2)),
				hi_eq = _mm256_shuffle_epi32(eq, _MM_SHUFFLE(3, 2, 3, 2)),
				lo_lt = _mm256_shuffle_epi32(lt, _MM_SHUFFLE(1, 0, 1, 0)),
				lo_eq = _mm256_shuffle_epi32(eq, _MM_SHUFFLE(1, 0, 1, 0));
	const uint32 mlt = MS64(_mm256_or_si256(hi_lt, _mm256_and_si256(hi_eq, lo_lt))),
				meq = MS64(_mm256_and_si256(hi_eq, lo_eq));

	return ak6_cmp_sel(op, (mlt & 1) | ((mlt >> 1) & 2), (meq & 1) | ((meq >> 1) & 2), 3);
}

K2 void
cmp_i128_k(uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c)
{
	const __m256i cv = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *) &c));

	PRED_BITS(out, n, 2, bits_i128(op, LD(v + j), cv), ak6_op_holds(op, ak6_i128_cmp(v[j], c)));
}

X2 void
ak6_cmp_i128_avx2(uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_i128_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

/* ---- a lookup table, a set, a string ---- */

/* The bytes of the table are gathered four at a time (a gather reads 4 bytes: SIFT_AK6_LUT_PAD) for the lanes in range */
K2 uint64
bits_lut(__m256i x, uint32 base, uint32 nlut, const uint8 *lut)
{
	const __m256i bias = SET32(0x80000000),
				idx = _mm256_sub_epi32(x, SET32(base)),
				inr = _mm256_cmpgt_epi32(SET32(nlut ^ 0x80000000), _mm256_xor_si256(idx, bias)),
				g = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), (const int *) lut, idx, inr, 1),
				zero = _mm256_cmpeq_epi32(_mm256_and_si256(g, SET32(0xFF)), _mm256_setzero_si256());

	return MS32(_mm256_andnot_si256(zero, inr));
}

X2 void
ak6_lut_u32_avx2(uint64 *out, const uint32 *v, uint16 n, uint32 base, uint32 nlut, const uint8 *lut)
{
	PRED_BITS(out, n, 8, bits_lut(LD(v + j), base, nlut, lut), (uint32) (v[j] - base) < nlut && lut[v[j] - base] != 0);
}

K2 uint64
bits_in_set(__m256i x, const int64 *set, uint8 nset)
{
	__m256i		acc = _mm256_setzero_si256();

	for (int k = 0; k < nset; k++)
		acc = _mm256_or_si256(acc, _mm256_cmpeq_epi64(x, SET64(set[k])));
	return MS64(acc);
}

X2 void
ak6_in_set_i64_avx2(uint64 *out, const int64 *v, uint16 n, const int64 *set, uint8 nset)
{
	PRED_BITS(out, n, 4, bits_in_set(LD(v + j), set, nset), ({ bool in_ = false; for (int k = 0; k < nset; k++) in_ |= v[j] == set[k]; in_; }));
}

/*
 * Four views, two vectors of two: the equal halves of a view (length and prefix, tail) say it, and only a view of more than 12 bytes with an
 * equal first half is compared by content
 */
K2 uint64
bits_strview(const SiftStrView *v, __m256i t, bool tlong, const SiftStrView *target)
{
	const uint32 e = MS64(_mm256_cmpeq_epi64(LD(v), t)) | (MS64(_mm256_cmpeq_epi64(LD(v + 2), t)) << 4);

	if (!tlong)
		return _pext_u32(e & (e >> 1), 0x55);
	return ((e & 1) && ak6_sv_eq(&v[0], target)) | ((uint64) ((e & 4) && ak6_sv_eq(&v[1], target)) << 1) |
		((uint64) ((e & 16) && ak6_sv_eq(&v[2], target)) << 2) | ((uint64) ((e & 64) && ak6_sv_eq(&v[3], target)) << 3);
}

X2 void
ak6_cmp_strview_eq_avx2(uint64 *out, const SiftStrView *v, uint16 n, SiftStrView target)
{
	const __m256i t = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *) &target));
	const bool	tlong = target.len > 12;

	PRED_BITS(out, n, 4, bits_strview(v + j, t, tlong, &target), ak6_sv_eq(&v[j], &target));
}

/* ---- the run boundaries ---- */

/* The bits of rows j..j+4, j >= 1: v[row] differs from the row before; the first four rows are the scalar rows */
K2 uint64
bits_run(const int64 *v, int j)
{
	return j == 0 ? (uint64) (1 | (v[1] != v[0]) << 1 | (v[2] != v[1]) << 2 | (v[3] != v[2]) << 3)
		: (~MS64(_mm256_cmpeq_epi64(LD(v + j), LD(v + j - 1)))) & 0xF;
}

X2 void
ak6_stream_run_ne_i64_avx2(uint64 *out, const int64 *v, const uint64 *valid, uint16 n)
{
	const int	nw = AK6_NW(n);

	PRED_BITS(out, n, 4, bits_run(v, j), j == 0 || v[j] != v[j - 1]);
	if (valid)
		for (int w = 0; w < nw; w++)
		{
			const uint64 vw = valid[w],
						prev = (vw << 1) | (w ? valid[w - 1] >> 63 : 0);

			out[w] = ((vw ^ prev) | (vw & out[w])) & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0));
		}
	if (nw)
		out[0] |= 1;
}

#endif							/* __x86_64__ */
