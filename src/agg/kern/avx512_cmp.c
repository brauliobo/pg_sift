/*
 * agg/kern/avx512_cmp.c - the AVX-512 predicates of the 2048-row engine (agg/kern/cmp.c is the specification): a compare into a mask
 * register gives the bits of 8 or 16 rows directly, the less-than and equal masks give the bits of every operator (ak6_cmp_sel), and the
 * operator is a constant of each copy of the body. Doubles take the float8 order from two masks (the NaNs of each side); a 128-bit value
 * is compared on the mask registers of its two halves (unsigned low, signed high). The rows that do not fill a vector are the scalar rows.
 */
#include "core/host.h"

#include "agg/kern/avx512.h"

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
#define FOR_OPS(CASE)	CASE(SCMP_EQ) CASE(SCMP_NE) CASE(SCMP_LT) CASE(SCMP_LE) CASE(SCMP_GT) CASE(SCMP_GE)

K5 uint64
bits_i32(SiftCmpOp op, __m512i x, __m512i y)
{
	return ak6_cmp_sel(op, _mm512_cmplt_epi32_mask(x, y), _mm512_cmpeq_epi32_mask(x, y), 0xFFFF);
}

K5 uint64
bits_i64(SiftCmpOp op, __m512i x, __m512i y)
{
	return ak6_cmp_sel(op, _mm512_cmplt_epi64_mask(x, y), _mm512_cmpeq_epi64_mask(x, y), 0xFF);
}

/* yn: the lanes of y that are NaN */
K5 uint64
bits_f64(SiftCmpOp op, __m512d x, __m512d y, __mmask8 yn)
{
	const __mmask8 xn = _mm512_cmp_pd_mask(x, x, _CMP_UNORD_Q),
				lt = _mm512_cmp_pd_mask(x, y, _CMP_LT_OQ) | (yn & ~xn),
				eq = _mm512_cmp_pd_mask(x, y, _CMP_EQ_OQ) | (xn & yn);

	return ak6_cmp_sel(op, lt, eq, 0xFF);
}

K5 void
cmp_i32_k(uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c)
{
	const __m512i cv = SET32(c);

	PRED_BITS(out, n, 16, bits_i32(op, LD(v + j), cv), ak6_op_holds(op, THREE(v[j], c)));
}

K5 void
cmp_i64_k(uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c)
{
	const __m512i cv = SET64(c);

	PRED_BITS(out, n, 8, bits_i64(op, LD(v + j), cv), ak6_op_holds(op, THREE(v[j], c)));
}

K5 void
cmp_f64_k(uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c)
{
	const __m512d cv = _mm512_set1_pd(c);
	const __mmask8 cn = c != c ? 0xFF : 0;

	PRED_BITS(out, n, 8, bits_f64(op, LDPD(v + j), cv, cn), ak6_op_holds(op, ak6_f8_cmp(v[j], c)));
}

K5 void
cmp2_i32_k(uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op)
{
	PRED_BITS(out, n, 16, bits_i32(op, LD(a + j), LD(b + j)), ak6_op_holds(op, THREE(a[j], b[j])));
}

K5 void
cmp2_i64_k(uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op)
{
	PRED_BITS(out, n, 8, bits_i64(op, LD(a + j), LD(b + j)), ak6_op_holds(op, THREE(a[j], b[j])));
}

K5 void
cmp2_f64_k(uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op)
{
	PRED_BITS(out, n, 8, bits_f64(op, LDPD(a + j), LDPD(b + j), _mm512_cmp_pd_mask(LDPD(b + j), LDPD(b + j), _CMP_UNORD_Q)),
			  ak6_op_holds(op, ak6_f8_cmp(a[j], b[j])));
}

X5 void
ak6_cmp_i32_avx512(uint64 *out, const int32 *v, uint16 n, SiftCmpOp op, int32 c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_i32_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X5 void
ak6_cmp_i64_avx512(uint64 *out, const int64 *v, uint16 n, SiftCmpOp op, int64 c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_i64_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X5 void
ak6_cmp_f64_avx512(uint64 *out, const double *v, uint16 n, SiftCmpOp op, double c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_f64_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X5 void
ak6_cmp2_i32_avx512(uint64 *out, const int32 *a, const int32 *b, uint16 n, SiftCmpOp op)
{
	switch (op)
	{
#define CASE(o) case o: cmp2_i32_k(out, a, b, n, o); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X5 void
ak6_cmp2_i64_avx512(uint64 *out, const int64 *a, const int64 *b, uint16 n, SiftCmpOp op)
{
	switch (op)
	{
#define CASE(o) case o: cmp2_i64_k(out, a, b, n, o); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

X5 void
ak6_cmp2_f64_avx512(uint64 *out, const double *a, const double *b, uint16 n, SiftCmpOp op)
{
	switch (op)
	{
#define CASE(o) case o: cmp2_f64_k(out, a, b, n, o); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

/* ---- int128: four values a vector, the lanes (low, high, ...) ---- */

K5 uint64
bits_i128(SiftCmpOp op, __m512i x, __m512i c)
{
	const __mmask8 mu = _mm512_cmplt_epu64_mask(x, c),
				ms = _mm512_cmplt_epi64_mask(x, c),
				me = _mm512_cmpeq_epi64_mask(x, c),
				hi_eq = (me >> 1) & 0x55,
				lt = ((ms >> 1) & 0x55) | (hi_eq & (mu & 0x55)),
				eq = hi_eq & (me & 0x55);

	return ak6_cmp_sel(op, _pext_u32(lt, 0x55), _pext_u32(eq, 0x55), 0xF);
}

K5 void
cmp_i128_k(uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c)
{
	const __m512i cv = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *) &c));

	PRED_BITS(out, n, 4, bits_i128(op, LD(v + j), cv), ak6_op_holds(op, ak6_i128_cmp(v[j], c)));
}

X5 void
ak6_cmp_i128_avx512(uint64 *out, const SiftI128 *v, uint16 n, SiftCmpOp op, SiftI128 c)
{
	switch (op)
	{
#define CASE(o) case o: cmp_i128_k(out, v, n, o, c); return;
		FOR_OPS(CASE)
#undef CASE
	}
}

/* ---- a lookup table, a set, a string ---- */

/* The bytes of the table are gathered sixteen at a time (a gather reads 4 bytes: SIFT_AK6_LUT_PAD) for the lanes in range */
X5 void
ak6_lut_u32_avx512(uint64 *out, const uint32 *v, uint16 n, uint32 base, uint32 nlut, const uint8 *lut)
{
	const __m512i vbase = SET32(base),
				vn = SET32(nlut),
				ff = SET32(0xFF);

	PRED_BITS(out, n, 16,
			  ({
				  const __m512i idx = _mm512_sub_epi32(LD(v + j), vbase);
				  const __mmask16 inr = _mm512_cmplt_epu32_mask(idx, vn);

				  _mm512_mask_test_epi32_mask(inr, _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), inr, idx, lut, 1), ff);
			  }),
			  (uint32) (v[j] - base) < nlut && lut[v[j] - base] != 0);
}

X5 void
ak6_in_set_i64_avx512(uint64 *out, const int64 *v, uint16 n, const int64 *set, uint8 nset)
{
	PRED_BITS(out, n, 8,
			  ({
				  const __m512i x = LD(v + j);
				  __mmask8 acc = 0;

				  for (int k = 0; k < nset; k++)
					  acc |= _mm512_cmpeq_epi64_mask(x, SET64(set[k]));
				  acc;
			  }),
			  ({ bool in_ = false; for (int k = 0; k < nset; k++) in_ |= v[j] == set[k]; in_; }));
}

/* Four views a vector: the equal halves of a pair say it, and only a view of more than 12 bytes with an equal first half is compared by content */
K5 uint64
bits_strview(const SiftStrView *v, __m512i t, bool tlong, const SiftStrView *target)
{
	const uint32 e = _mm512_cmpeq_epi64_mask(LD(v), t);

	if (!tlong)
		return _pext_u32(e & (e >> 1), 0x55);
	return ((e & 1) && ak6_sv_eq(&v[0], target)) | ((uint64) ((e & 4) && ak6_sv_eq(&v[1], target)) << 1) |
		((uint64) ((e & 16) && ak6_sv_eq(&v[2], target)) << 2) | ((uint64) ((e & 64) && ak6_sv_eq(&v[3], target)) << 3);
}

X5 void
ak6_cmp_strview_eq_avx512(uint64 *out, const SiftStrView *v, uint16 n, SiftStrView target)
{
	const __m512i t = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *) &target));
	const bool	tlong = target.len > 12;

	PRED_BITS(out, n, 4, bits_strview(v + j, t, tlong, &target), ak6_sv_eq(&v[j], &target));
}

/* ---- the run boundaries ---- */

/* The bits of rows j..j+8: v[row] differs from the row before; the first eight rows are the scalar rows */
K5 uint64
bits_run(const int64 *v, int j)
{
	uint64		b = 1;

	if (j)
		return _mm512_cmpneq_epi64_mask(LD(v + j), LD(v + j - 1));
	for (int r = 1; r < 8; r++)
		b |= (uint64) (v[r] != v[r - 1]) << r;
	return b;
}

X5 void
ak6_stream_run_ne_i64_avx512(uint64 *out, const int64 *v, const uint64 *valid, uint16 n)
{
	const int	nw = AK6_NW(n);

	PRED_BITS(out, n, 8, bits_run(v, j), j == 0 || v[j] != v[j - 1]);
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
