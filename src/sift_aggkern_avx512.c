/*
 * sift_aggkern_avx512.c - AVX-512 (F/BW/VL/DQ/VBMI2/VPOPCNTDQ) aggregation operators. Compares produce mask registers directly
 * (16 int32 or 8 int64 / double rows per instruction), selections are applied as merging masked min/max and masked loads,
 * selection vectors are VPCOMPRESSB of the bit positions (register form only: the memory forms are microcoded on Zen 4),
 * lookups and gathers are masked VPGATHER, popcounts are VPOPCNTQ. Results are bit-identical to the scalar table
 * (sift_aggkern.c). Slots that gathers or a per-group scatter do not speed up (gather_u64, count_gid, sum_gid_*) are the
 * scalar functions.
 */
#include "sift_host.h"

#include "sift_aggkern_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K5 static inline __attribute__((always_inline)) SIFT_TARGET_AVX512
#define F5 static SIFT_TARGET_AVX512

#define L5(p)		_mm512_loadu_si512((const void *) (p))
#define S5(p, x)	_mm512_storeu_si512((void *) (p), x)
#define LPD(p)		_mm512_loadu_pd((const void *) (p))
#define SEL16(mask, i)	((__mmask16) ((mask)[(i) >> 6] >> ((i) & 63)))
#define SEL8(mask, i)	((__mmask8) ((mask)[(i) >> 6] >> ((i) & 63)))

/* ---- predicates ---- */

F5 void
cmp_i32_avx512(uint64 *out, const int32 *v, int n, SiftCmpOp op, int32 c)
{
	const __m512i vc = _mm512_set1_epi32(c);

#define CMP32(IMM) AGK_BITS(out, n, 16, 0, i, _mm512_cmp_epi32_mask(L5(v + i), vc, IMM))
	switch (op)
	{
		case SCMP_EQ:
			CMP32(_MM_CMPINT_EQ);
			break;
		case SCMP_NE:
			CMP32(_MM_CMPINT_NE);
			break;
		case SCMP_LT:
			CMP32(_MM_CMPINT_LT);
			break;
		case SCMP_LE:
			CMP32(_MM_CMPINT_LE);
			break;
		case SCMP_GT:
			CMP32(_MM_CMPINT_NLE);
			break;
		default:
			CMP32(_MM_CMPINT_NLT);
			break;
	}
#undef CMP32
}

F5 void
cmp_i64_avx512(uint64 *out, const int64 *v, int n, SiftCmpOp op, int64 c)
{
	const __m512i vc = _mm512_set1_epi64(c);

#define CMP64(IMM) AGK_BITS(out, n, 8, 0, i, _mm512_cmp_epi64_mask(L5(v + i), vc, IMM))
	switch (op)
	{
		case SCMP_EQ:
			CMP64(_MM_CMPINT_EQ);
			break;
		case SCMP_NE:
			CMP64(_MM_CMPINT_NE);
			break;
		case SCMP_LT:
			CMP64(_MM_CMPINT_LT);
			break;
		case SCMP_LE:
			CMP64(_MM_CMPINT_LE);
			break;
		case SCMP_GT:
			CMP64(_MM_CMPINT_NLE);
			break;
		default:
			CMP64(_MM_CMPINT_NLT);
			break;
	}
#undef CMP64
}

/* Same predicate mapping as the AVX2 level: "not <=" / "not <" are true for a NaN row; a NaN constant is the scalar function's */
F5 void
cmp_f64_avx512(uint64 *out, const double *v, int n, SiftCmpOp op, double c)
{
	const __m512d vc = _mm512_set1_pd(c);

	if (c != c)
	{
		agk_cmp_f64_scalar(out, v, n, op, c);
		return;
	}
#define CMPF(IMM) AGK_BITS(out, n, 8, 0, i, _mm512_cmp_pd_mask(LPD(v + i), vc, IMM))
	switch (op)
	{
		case SCMP_EQ:
			CMPF(_CMP_EQ_OQ);
			break;
		case SCMP_NE:
			CMPF(_CMP_NEQ_UQ);
			break;
		case SCMP_LT:
			CMPF(_CMP_LT_OQ);
			break;
		case SCMP_LE:
			CMPF(_CMP_LE_OQ);
			break;
		case SCMP_GT:
			CMPF(_CMP_NLE_UQ);
			break;
		default:
			CMPF(_CMP_NLT_UQ);
			break;
	}
#undef CMPF
}

F5 void
between_i32_avx512(uint64 *out, const int32 *v, int n, int32 lo, int32 hi)
{
	const __m512i vlo = _mm512_set1_epi32(lo),
				d = _mm512_set1_epi32((uint32) hi - (uint32) lo);

	if (lo > hi)
	{
		memset(out, 0, AGK_NW(n) * 8);
		return;
	}
	AGK_BITS(out, n, 16, 0, i, _mm512_cmple_epu32_mask(_mm512_sub_epi32(L5(v + i), vlo), d));
}

F5 void
between_i64_avx512(uint64 *out, const int64 *v, int n, int64 lo, int64 hi)
{
	const __m512i vlo = _mm512_set1_epi64(lo),
				d = _mm512_set1_epi64((uint64) hi - (uint64) lo);

	if (lo > hi)
	{
		memset(out, 0, AGK_NW(n) * 8);
		return;
	}
	AGK_BITS(out, n, 8, 0, i, _mm512_cmple_epu64_mask(_mm512_sub_epi64(L5(v + i), vlo), d));
}

F5 void
between_f64_avx512(uint64 *out, const double *v, int n, double lo, double hi)
{
	const __m512d vlo = _mm512_set1_pd(lo),
				vhi = _mm512_set1_pd(hi);

	if (lo != lo || hi != hi)
	{
		agk_between_f64_scalar(out, v, n, lo, hi);
		return;
	}
	AGK_BITS(out, n, 8, 0, i,
			 _mm512_mask_cmp_pd_mask(_mm512_cmp_pd_mask(LPD(v + i), vlo, _CMP_GE_OQ), LPD(v + i), vhi, _CMP_LE_OQ));
}

/* Rows whose gathered dword has a nonzero low byte (the LUT has SIFT_AGG_PAD readable bytes after it) */
K5 __mmask16
nonzero_byte(__m512i g)
{
	return _mm512_test_epi32_mask(g, _mm512_set1_epi32(0xFF));
}

F5 void
lut_code_avx512(uint64 *out, const uint32 *code, int n, const uint8 *lut)
{
	const __m512i z = _mm512_setzero_si512();

	AGK_BITS2(out, n, 16, 0, i, nonzero_byte(_mm512_i32gather_epi32(L5(code + i), lut, 1)),
			  nonzero_byte(_mm512_mask_i32gather_epi32(z, AGK_LANES(n - i, 16),
													   _mm512_maskz_loadu_epi32(AGK_LANES(n - i, 16), code + i), lut, 1)));
}

/* Lanes outside the table are masked out of the gather (zero, so they read as "not set") */
K5 __mmask16
lut_i32_step(const uint8 *lut, __m512i x, __m512i vb, __m512i vn)
{
	__m512i		t = _mm512_sub_epi32(x, vb);

	return nonzero_byte(_mm512_mask_i32gather_epi32(_mm512_setzero_si512(), _mm512_cmplt_epu32_mask(t, vn), t, lut, 1));
}

F5 void
lut_i32_avx512(uint64 *out, const int32 *v, int n, int32 base, uint32 nlut, const uint8 *lut)
{
	const __m512i vb = _mm512_set1_epi32(base),
				vn = _mm512_set1_epi32(nlut);

	AGK_BITS(out, n, 16, 0, i, lut_i32_step(lut, L5(v + i), vb, vn));
}

F5 void
bytes_mask_avx512(uint64 *out, const uint8 *b, int n)
{
	const int	nf = n >> 6,
				rem = n & 63;

	for (int w = 0; w < nf; w++)
	{
		__m512i		x = L5(b + w * 64);

		out[w] = _mm512_test_epi8_mask(x, x);
	}
	if (rem)
	{
		__m512i		x = _mm512_maskz_loadu_epi8(AGK_LOW(rem), b + nf * 64);

		out[nf] = _mm512_test_epi8_mask(x, x);
	}
}

/* ---- bitmaps ---- */

#define DEF_BITOP(fn, VOP) \
F5 void \
fn(uint64 *dst, const uint64 *a, int nw) \
{ \
	int			i = 0; \
\
	for (; i + 8 <= nw; i += 8) \
		S5(dst + i, VOP(L5(dst + i), L5(a + i))); \
	if (i < nw) \
	{ \
		const __mmask8 k = AGK_LOW(nw - i); \
\
		_mm512_mask_storeu_epi64(dst + i, k, VOP(_mm512_maskz_loadu_epi64(k, dst + i), \
												 _mm512_maskz_loadu_epi64(k, a + i))); \
	} \
}

#define ANDNOT(d, a)	_mm512_andnot_si512(a, d)
DEF_BITOP(and_w_avx512, _mm512_and_si512)
DEF_BITOP(or_w_avx512, _mm512_or_si512)
DEF_BITOP(andnot_w_avx512, ANDNOT)

F5 int64
popcount_w_avx512(const uint64 *a, int nw)
{
	__m512i		acc = _mm512_setzero_si512();
	int			i = 0;

	for (; i + 8 <= nw; i += 8)
		acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(L5(a + i)));
	if (i < nw)
		acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(_mm512_maskz_loadu_epi64(AGK_LOW(nw - i), a + i)));
	return _mm512_reduce_add_epi64(acc);
}

/* bit e = bit parent[e] of rowmask: gather the dword that holds it (k: the lanes that exist), shift it down */
K5 __mmask16
row_bits(const uint64 *rowmask, __m512i p, __mmask16 k)
{
	__m512i		g = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), k, _mm512_srli_epi32(p, 5), rowmask, 4);

	return _mm512_test_epi32_mask(_mm512_srlv_epi32(g, _mm512_and_si512(p, _mm512_set1_epi32(31))), _mm512_set1_epi32(1));
}

F5 void
gather_bits_avx512(uint64 *out, const uint64 *rowmask, const uint32 *parent, int m)
{
	AGK_BITS2(out, m, 16, 0, e, row_bits(rowmask, L5(parent + e), 0xFFFF),
			  row_bits(rowmask, _mm512_maskz_loadu_epi32(AGK_LANES(m - e, 16), parent + e), AGK_LANES(m - e, 16)));
}

/* ---- selection vectors ---- */

/* One VPCOMPRESSB packs the positions of the bits of a word, four widening stores write them (the buffer has room for 64 more) */
F5 int
mask_sel_avx512(uint32 *sel, const uint64 *mask, int nw)
{
	const __m512i iota = _mm512_set_epi64(0x3F3E3D3C3B3A3938, 0x3736353433323130, 0x2F2E2D2C2B2A2928, 0x2726252423222120,
										  0x1F1E1D1C1B1A1918, 0x1716151413121110, 0x0F0E0D0C0B0A0908, 0x0706050403020100);
	int			cnt = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		m = mask[w];
		__m512i		pos,
					base;

		if (__builtin_popcountll(m) <= AGK_SEL_SPARSE)
		{
			for (; m; m &= m - 1)
				sel[cnt++] = w * 64 + __builtin_ctzll(m);
			continue;
		}
		pos = _mm512_maskz_compress_epi8(m, iota);
		base = _mm512_set1_epi32(w * 64);
		S5(sel + cnt, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_castsi512_si128(pos)), base));
		S5(sel + cnt + 16, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(pos, 1)), base));
		S5(sel + cnt + 32, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(pos, 2)), base));
		S5(sel + cnt + 48, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(pos, 3)), base));
		cnt += __builtin_popcountll(m);
	}
	return cnt;
}

F5 void
gather_u32_avx512(uint32 *dst, const uint32 *src, const uint32 *sel, int nsel)
{
	int			j = 0;

	for (; j + 16 <= nsel; j += 16)
		S5(dst + j, _mm512_i32gather_epi32(L5(sel + j), src, 4));
	if (j < nsel)
	{
		const __mmask16 k = AGK_LOW(nsel - j);

		_mm512_mask_storeu_epi32(dst + j, k,
								 _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), k,
															 _mm512_maskz_loadu_epi32(k, sel + j), src, 4));
	}
}

/* ---- group ids ---- */

F5 void
gid_code_avx512(uint32 *gid, const uint32 *code, const uint32 *map, int n, uint32 mult)
{
	const __m512i vm = _mm512_set1_epi32(mult),
				z = _mm512_setzero_si512();
	int			i = 0;

	for (; i + 16 <= n; i += 16)
		S5(gid + i, _mm512_add_epi32(L5(gid + i), _mm512_mullo_epi32(_mm512_i32gather_epi32(L5(code + i), map, 4), vm)));
	if (i < n)
	{
		const __mmask16 k = AGK_LOW(n - i);
		__m512i		g = _mm512_mask_i32gather_epi32(z, k, _mm512_maskz_loadu_epi32(k, code + i), map, 4);

		_mm512_mask_storeu_epi32(gid + i, k, _mm512_add_epi32(_mm512_maskz_loadu_epi32(k, gid + i), _mm512_mullo_epi32(g, vm)));
	}
}

F5 void
gid_i32_avx512(uint32 *gid, const int32 *v, int32 base, const uint32 *map, int n, uint32 mult)
{
	const __m512i vm = _mm512_set1_epi32(mult),
				vb = _mm512_set1_epi32(base),
				z = _mm512_setzero_si512();
	int			i = 0;

	for (; i + 16 <= n; i += 16)
		S5(gid + i, _mm512_add_epi32(L5(gid + i),
									 _mm512_mullo_epi32(_mm512_i32gather_epi32(_mm512_sub_epi32(L5(v + i), vb), map, 4), vm)));
	if (i < n)
	{
		const __mmask16 k = AGK_LOW(n - i);
		__m512i		g = _mm512_mask_i32gather_epi32(z, k, _mm512_sub_epi32(_mm512_maskz_loadu_epi32(k, v + i), vb), map, 4);

		_mm512_mask_storeu_epi32(gid + i, k, _mm512_add_epi32(_mm512_maskz_loadu_epi32(k, gid + i), _mm512_mullo_epi32(g, vm)));
	}
}

/* ---- reductions ---- */

/* 16 rows per step: the two halves are sign-extended under their mask bits into two 8 x int64 accumulators */
K5 int64
sum_i32_t(const int32 *v, const uint64 *mask, int n, const bool masked)
{
	__m512i		a0 = _mm512_setzero_si512(),
				a1 = a0;

	for (int i = 0; i < n; i += 16)
	{
		const uint32 k = masked ? SEL16(mask, i) : (uint32) AGK_LANES(n - i, 16);

		a0 = _mm512_add_epi64(a0, _mm512_maskz_cvtepi32_epi64((__mmask8) k, _mm256_loadu_si256((const __m256i *) (v + i))));
		a1 = _mm512_add_epi64(a1, _mm512_maskz_cvtepi32_epi64((__mmask8) (k >> 8), _mm256_loadu_si256((const __m256i *) (v + i + 8))));
	}
	return _mm512_reduce_add_epi64(_mm512_add_epi64(a0, a1));
}

F5 int64
sum_i32_avx512(const int32 *v, const uint64 *mask, int n)
{
	return mask ? sum_i32_t(v, mask, n, true) : sum_i32_t(v, mask, n, false);
}

/*
 * The specified accumulators are eight chains of n / 8 dependent adds: plain adds on two ymm accumulators (the latency of a
 * merge-masked or a zmm add is longer, docs/KERNELS.md; a masked load costs more than a plain one, so only the rows of a
 * mask and the tail use it); an unselected lane is loaded as +0.0, which adds nothing (an accumulator is never -0.0).
 */
K5 double
sum_f64_t(const double *v, const uint64 *mask, int n, const bool masked)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	int			i = 0;

	for (; !masked && i + 8 <= n; i += 8)
	{
		a0 = _mm256_add_pd(a0, _mm256_loadu_pd(v + i));
		a1 = _mm256_add_pd(a1, _mm256_loadu_pd(v + i + 4));
	}
	for (; i < n; i += 8)
	{
		const uint32 k = masked ? SEL8(mask, i) : (uint32) AGK_LANES(n - i, 8);

		a0 = _mm256_add_pd(a0, _mm256_maskz_loadu_pd(k & 15, v + i));
		a1 = _mm256_add_pd(a1, _mm256_maskz_loadu_pd(k >> 4, v + i + 4));
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return agk_canon_nan(((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7])));
}

F5 double
sum_f64_avx512(const double *v, const uint64 *mask, int n)
{
	return mask ? sum_f64_t(v, mask, n, true) : sum_f64_t(v, mask, n, false);
}

K5 bool
minmax_i32_t(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n, const bool masked)
{
	__m512i		lo = _mm512_set1_epi32(INT32_MAX),
				hi = _mm512_set1_epi32(INT32_MIN);
	uint32		any = 0;
	int			i = 0;

	for (; !masked && i + 16 <= n; i += 16)
	{
		const __m512i x = L5(v + i);

		lo = _mm512_min_epi32(lo, x);
		hi = _mm512_max_epi32(hi, x);
	}
	for (; i < n; i += 16)
	{
		const __mmask16 k = masked ? SEL16(mask, i) : (__mmask16) AGK_LANES(n - i, 16);
		const __m512i x = L5(v + i);

		any |= k;
		lo = _mm512_mask_min_epi32(lo, k, lo, x);
		hi = _mm512_mask_max_epi32(hi, k, hi, x);
	}
	if (!masked ? n == 0 : !any)
		return false;
	*mn = _mm512_reduce_min_epi32(lo);
	*mx = _mm512_reduce_max_epi32(hi);
	return true;
}

F5 bool
minmax_i32_avx512(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n)
{
	return mask ? minmax_i32_t(mn, mx, v, mask, n, true) : minmax_i32_t(mn, mx, v, mask, n, false);
}

K5 bool
minmax_i64_t(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n, const bool masked)
{
	__m512i		lo = _mm512_set1_epi64(INT64_MAX),
				hi = _mm512_set1_epi64(INT64_MIN);
	uint32		any = 0;
	int			i = 0;

	for (; !masked && i + 8 <= n; i += 8)
	{
		const __m512i x = L5(v + i);

		lo = _mm512_min_epi64(lo, x);
		hi = _mm512_max_epi64(hi, x);
	}
	for (; i < n; i += 8)
	{
		const __mmask8 k = masked ? SEL8(mask, i) : (__mmask8) AGK_LANES(n - i, 8);
		const __m512i x = L5(v + i);

		any |= k;
		lo = _mm512_mask_min_epi64(lo, k, lo, x);
		hi = _mm512_mask_max_epi64(hi, k, hi, x);
	}
	if (!masked ? n == 0 : !any)
		return false;
	*mn = _mm512_reduce_min_epi64(lo);
	*mx = _mm512_reduce_max_epi64(hi);
	return true;
}

F5 bool
minmax_i64_avx512(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n)
{
	return mask ? minmax_i64_t(mn, mx, v, mask, n, true) : minmax_i64_t(mn, mx, v, mask, n, false);
}

/*
 * VMINPD / VMAXPD return their second operand when the first is a NaN, so with the running value second a NaN never enters
 * it; NaN rows are counted on the side. The minimum is NaN only when every selected row is (the running minimum then never
 * left +Inf and the maximum never left -Inf: lo <= hi tells that a non-NaN row was seen), the maximum when any is.
 * Four accumulator pairs, 32 rows a step: the chain of one is too long for the loop to go at the load rate. A group past
 * the end has an empty mask, so its load touches nothing.
 */
K5 bool
minmax_f64_t(double *mn, double *mx, const double *v, const uint64 *mask, int n, const bool masked)
{
	__m512d		lo[4],
				hi[4];
	uint32		anynan = 0,
				any = 0;
	int			i = 0;

	for (int g = 0; g < 4; g++)
	{
		lo[g] = _mm512_set1_pd(INFINITY);
		hi[g] = _mm512_set1_pd(-INFINITY);
	}
	for (; !masked && i + 32 <= n; i += 32)
	{
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
		{
			const __m512d x = LPD(v + i + 8 * g);

			anynan |= _mm512_cmp_pd_mask(x, x, _CMP_UNORD_Q);
			lo[g] = _mm512_min_pd(x, lo[g]);
			hi[g] = _mm512_max_pd(x, hi[g]);
		}
	}
	for (; i < n; i += 32)
	{
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
		{
			const int	r = n - i - 8 * g;
			const __mmask8 k = r <= 0 ? 0 : masked ? SEL8(mask, i + 8 * g) : (__mmask8) AGK_LANES(r, 8);
			const __m512d x = _mm512_maskz_loadu_pd(k, v + i + 8 * g);

			any |= k;
			anynan |= _mm512_mask_cmp_pd_mask(k, x, x, _CMP_UNORD_Q);
			lo[g] = _mm512_mask_min_pd(lo[g], k, x, lo[g]);
			hi[g] = _mm512_mask_max_pd(hi[g], k, x, hi[g]);
		}
	}
	if (!masked ? n == 0 : !any)
		return false;
	for (int g = 1; g < 4; g++)
	{
		lo[0] = _mm512_min_pd(lo[g], lo[0]);
		hi[0] = _mm512_max_pd(hi[g], hi[0]);
	}
	{
		const double l = _mm512_reduce_min_pd(lo[0]),
					h = _mm512_reduce_max_pd(hi[0]);

		*mn = agk_canon_minmax(l <= h ? l : __builtin_nan(""));
		*mx = agk_canon_minmax(anynan ? __builtin_nan("") : h);
	}
	return true;
}

F5 bool
minmax_f64_avx512(double *mn, double *mx, const double *v, const uint64 *mask, int n)
{
	return mask ? minmax_f64_t(mn, mx, v, mask, n, true) : minmax_f64_t(mn, mx, v, mask, n, false);
}

/*
 * The selected values are compressed to the front of a buffer behind the previous value (VPCOMPRESSD), then every value is
 * compared with its predecessor in the buffer. Without a mask the rows are compared with themselves shifted by one.
 * The vector has at most SIFT_AGG_VEC rows.
 */
F5 int64
count_changes_avx512(const uint32 *v, const uint64 *mask, int n, uint32 *prev)
{
	uint32		buf[SIFT_AGG_VEC + 32];
	int64		cnt = 0;
	int			c = 0;

	if (!mask)
	{
		if (n == 0)
			return 0;
		cnt = v[0] != *prev;
		for (int i = 1; i < n; i += 16)
			cnt += __builtin_popcount(_mm512_cmpneq_epi32_mask(L5(v + i), L5(v + i - 1)) & AGK_LANES(n - i, 16));
		*prev = v[n - 1];
		return cnt;
	}
	buf[0] = *prev;
	for (int i = 0; i < n; i += 16)
	{
		const __mmask16 k = SEL16(mask, i);

		S5(buf + 1 + c, _mm512_maskz_compress_epi32(k, L5(v + i)));
		c += __builtin_popcount(k);
	}
	S5(buf + 1 + c, _mm512_setzero_si512());
	for (int j = 0; j < c; j += 16)
		cnt += __builtin_popcount(_mm512_cmpneq_epi32_mask(L5(buf + j), L5(buf + j + 1)) & AGK_LANES(c - j, 16));
	*prev = buf[c];
	return cnt;
}

#define AGK_INIT5(s)	.s = s##_avx512,

const SiftAggKernels sift_aggkern_avx512 = {
	AGK_INIT5(cmp_i32) AGK_INIT5(cmp_i64) AGK_INIT5(cmp_f64)
	AGK_INIT5(between_i32) AGK_INIT5(between_i64) AGK_INIT5(between_f64)
	AGK_INIT5(lut_code) AGK_INIT5(lut_i32) AGK_INIT5(bytes_mask)
	AGK_INIT5(and_w) AGK_INIT5(or_w) AGK_INIT5(andnot_w) AGK_INIT5(popcount_w) AGK_INIT5(gather_bits)
	AGK_INIT5(mask_sel) AGK_INIT5(gather_u32) AGK_INIT5(gid_code) AGK_INIT5(gid_i32)
	AGK_INIT5(sum_i32) AGK_INIT5(sum_f64) AGK_INIT5(minmax_i32) AGK_INIT5(minmax_i64) AGK_INIT5(minmax_f64)
	.gather_u64 = agk_gather_u64_scalar,
	.count_gid = agk_count_gid_scalar,
	.sum_gid_i32 = agk_sum_gid_i32_scalar,
	.sum_gid_f64 = agk_sum_gid_f64_scalar,
	AGK_INIT5(count_changes)
};

#endif							/* __x86_64__ */
