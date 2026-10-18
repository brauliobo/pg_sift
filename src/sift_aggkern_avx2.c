/*
 * sift_aggkern_avx2.c - AVX2 aggregation operators. Predicates compare 8 (int32), 4 (int64, double) or 32 (bytes) rows per
 * instruction and take the bits with MOVMSK; lookups use VPGATHER (masked where a lane may not be touched); selection vectors
 * expand a byte of the mask through a 256 x 8 position table; reductions keep several independent accumulators. Results are
 * bit-identical to the scalar table (sift_aggkern.c): float compares are mapped to unordered predicates so that NaN is greater
 * than everything, and the float sum keeps the eight accumulators of the specification (two ymm). Slots that gathers or a
 * per-group scatter do not speed up (gather_u32, gather_u64, gid_code, gid_i32, count_gid, sum_gid_*) are the scalar functions.
 */
#include "sift_host.h"

#include "sift_aggkern_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K2 static inline __attribute__((always_inline)) SIFT_TARGET_AVX2
#define F2 static SIFT_TARGET_AVX2

#define LD(p)		_mm256_loadu_si256((const __m256i *) (p))
#define ST(p, x)	_mm256_storeu_si256((__m256i *) (p), x)
#define LDPD(p)		_mm256_loadu_pd((const double *) (p))
#define ALL1		(~UINT64CONST(0))
#define BYTE(mask, i)	((uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & 0xFF)
#define NIBBLE(mask, i)	((uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & 0xF)

/* Position of the j-th set bit of m (0 past the last), lane j of row m */
#define R4(F, m)	F(m), F(m + 1), F(m + 2), F(m + 3)
#define R16(F, m)	R4(F, m), R4(F, m + 4), R4(F, m + 8), R4(F, m + 12)
#define R64(F, m)	R16(F, m), R16(F, m + 16), R16(F, m + 32), R16(F, m + 48)
#define R256(F)		R64(F, 0), R64(F, 64), R64(F, 128), R64(F, 192)
#define BELOW(m, j)	__builtin_popcount((m) & ((1 << (j)) - 1))
#define POS(m, j, b)	((((m) >> (b)) & 1) && BELOW(m, b) == (j) ? (b) : 0)
#define LANE(m, j)	(POS(m, j, 0) + POS(m, j, 1) + POS(m, j, 2) + POS(m, j, 3) + \
					 POS(m, j, 4) + POS(m, j, 5) + POS(m, j, 6) + POS(m, j, 7))
#define POSROW(m)	{LANE(m, 0), LANE(m, 1), LANE(m, 2), LANE(m, 3), LANE(m, 4), LANE(m, 5), LANE(m, 6), LANE(m, 7)}
static const uint8 lut_pos[256][8] = {R256(POSROW)};

K2 uint32
ms32(__m256i x)
{
	return _mm256_movemask_ps(_mm256_castsi256_ps(x));
}

K2 uint32
ms64(__m256i x)
{
	return _mm256_movemask_pd(_mm256_castsi256_pd(x));
}

/* All-ones 32-bit lanes for the set bits of k (8 bits) */
K2 __m256i
lanes32(uint32 k)
{
	const __m256i bit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);

	return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(k), bit), bit);
}

/* All-ones 64-bit lanes for the set bits of k (4 bits): one load from a 16-entry table */
#define LM(n)		{-(int64) ((n) & 1), -(int64) (((n) >> 1) & 1), -(int64) (((n) >> 2) & 1), -(int64) (((n) >> 3) & 1)}
static const int64 lane_tab[16][4] __attribute__((aligned(32))) = {R16(LM, 0)};

K2 __m256i
lanes64(uint32 k)
{
	return _mm256_load_si256((const __m256i *) lane_tab[k]);
}

/* Eight 32-bit entries, of which only the first r are read (the others are zero) */
K2 __m256i
ld_n32(const void *p, int r)
{
	return _mm256_maskload_epi32((const int *) p, lanes32(AGK_LANES(r, 8)));
}

K2 int64
hsum64(__m256i x)
{
	__m128i		s = _mm_add_epi64(_mm256_castsi256_si128(x), _mm256_extracti128_si256(x, 1));

	return _mm_cvtsi128_si64(s) + _mm_extract_epi64(s, 1);
}

/* ---- predicates ---- */

F2 void
cmp_i32_avx2(uint64 *out, const int32 *v, int n, SiftCmpOp op, int32 c)
{
	const __m256i vc = _mm256_set1_epi32(c);

	switch (op)
	{
		case SCMP_EQ:
			AGK_BITS(out, n, 8, 0, i, ms32(_mm256_cmpeq_epi32(LD(v + i), vc)));
			break;
		case SCMP_NE:
			AGK_BITS(out, n, 8, ALL1, i, ms32(_mm256_cmpeq_epi32(LD(v + i), vc)));
			break;
		case SCMP_LT:
			AGK_BITS(out, n, 8, 0, i, ms32(_mm256_cmpgt_epi32(vc, LD(v + i))));
			break;
		case SCMP_LE:
			AGK_BITS(out, n, 8, ALL1, i, ms32(_mm256_cmpgt_epi32(LD(v + i), vc)));
			break;
		case SCMP_GT:
			AGK_BITS(out, n, 8, 0, i, ms32(_mm256_cmpgt_epi32(LD(v + i), vc)));
			break;
		default:
			AGK_BITS(out, n, 8, ALL1, i, ms32(_mm256_cmpgt_epi32(vc, LD(v + i))));
			break;
	}
}

F2 void
cmp_i64_avx2(uint64 *out, const int64 *v, int n, SiftCmpOp op, int64 c)
{
	const __m256i vc = _mm256_set1_epi64x(c);

	switch (op)
	{
		case SCMP_EQ:
			AGK_BITS(out, n, 4, 0, i, ms64(_mm256_cmpeq_epi64(LD(v + i), vc)));
			break;
		case SCMP_NE:
			AGK_BITS(out, n, 4, ALL1, i, ms64(_mm256_cmpeq_epi64(LD(v + i), vc)));
			break;
		case SCMP_LT:
			AGK_BITS(out, n, 4, 0, i, ms64(_mm256_cmpgt_epi64(vc, LD(v + i))));
			break;
		case SCMP_LE:
			AGK_BITS(out, n, 4, ALL1, i, ms64(_mm256_cmpgt_epi64(LD(v + i), vc)));
			break;
		case SCMP_GT:
			AGK_BITS(out, n, 4, 0, i, ms64(_mm256_cmpgt_epi64(LD(v + i), vc)));
			break;
		default:
			AGK_BITS(out, n, 4, ALL1, i, ms64(_mm256_cmpgt_epi64(vc, LD(v + i))));
			break;
	}
}

/*
 * PostgreSQL float8 order against a constant that is not NaN: ordered predicates are false for a NaN row, the "not <=" and
 * "not <" ones are true for it (NaN is greater than everything). A NaN constant (rare) is the scalar function's.
 */
F2 void
cmp_f64_avx2(uint64 *out, const double *v, int n, SiftCmpOp op, double c)
{
	const __m256d vc = _mm256_set1_pd(c);

	if (c != c)
	{
		agk_cmp_f64_scalar(out, v, n, op, c);
		return;
	}
	switch (op)
	{
		case SCMP_EQ:
			AGK_BITS(out, n, 4, 0, i, _mm256_movemask_pd(_mm256_cmp_pd(LDPD(v + i), vc, _CMP_EQ_OQ)));
			break;
		case SCMP_NE:
			AGK_BITS(out, n, 4, 0, i, _mm256_movemask_pd(_mm256_cmp_pd(LDPD(v + i), vc, _CMP_NEQ_UQ)));
			break;
		case SCMP_LT:
			AGK_BITS(out, n, 4, 0, i, _mm256_movemask_pd(_mm256_cmp_pd(LDPD(v + i), vc, _CMP_LT_OQ)));
			break;
		case SCMP_LE:
			AGK_BITS(out, n, 4, 0, i, _mm256_movemask_pd(_mm256_cmp_pd(LDPD(v + i), vc, _CMP_LE_OQ)));
			break;
		case SCMP_GT:
			AGK_BITS(out, n, 4, 0, i, _mm256_movemask_pd(_mm256_cmp_pd(LDPD(v + i), vc, _CMP_NLE_UQ)));
			break;
		default:
			AGK_BITS(out, n, 4, 0, i, _mm256_movemask_pd(_mm256_cmp_pd(LDPD(v + i), vc, _CMP_NLT_UQ)));
			break;
	}
}

/* lo <= v <= hi as one unsigned compare: (uint32) (v - lo) <= hi - lo */
K2 uint32
in_range32(__m256i x, __m256i vlo, __m256i d)
{
	__m256i		t = _mm256_sub_epi32(x, vlo);

	return ms32(_mm256_cmpeq_epi32(_mm256_min_epu32(t, d), t));
}

F2 void
between_i32_avx2(uint64 *out, const int32 *v, int n, int32 lo, int32 hi)
{
	const __m256i vlo = _mm256_set1_epi32(lo),
				d = _mm256_set1_epi32((uint32) hi - (uint32) lo);

	if (lo > hi)
	{
		memset(out, 0, AGK_NW(n) * 8);
		return;
	}
	AGK_BITS(out, n, 8, 0, i, in_range32(LD(v + i), vlo, d));
}

/* AVX2 has no unsigned 64-bit compare: flip the sign bit of both sides, then signed "greater" is the complement */
F2 void
between_i64_avx2(uint64 *out, const int64 *v, int n, int64 lo, int64 hi)
{
	const __m256i vlo = _mm256_set1_epi64x(lo),
				sign = _mm256_set1_epi64x(INT64_MIN),
				d = _mm256_xor_si256(_mm256_set1_epi64x((uint64) hi - (uint64) lo), sign);

	if (lo > hi)
	{
		memset(out, 0, AGK_NW(n) * 8);
		return;
	}
	AGK_BITS(out, n, 4, ALL1, i,
			 ms64(_mm256_cmpgt_epi64(_mm256_xor_si256(_mm256_sub_epi64(LD(v + i), vlo), sign), d)));
}

F2 void
between_f64_avx2(uint64 *out, const double *v, int n, double lo, double hi)
{
	const __m256d vlo = _mm256_set1_pd(lo),
				vhi = _mm256_set1_pd(hi);

	if (lo != lo || hi != hi)
	{
		agk_between_f64_scalar(out, v, n, lo, hi);
		return;
	}
	AGK_BITS(out, n, 4, 0, i,
			 _mm256_movemask_pd(_mm256_and_pd(_mm256_cmp_pd(LDPD(v + i), vlo, _CMP_GE_OQ),
											  _mm256_cmp_pd(LDPD(v + i), vhi, _CMP_LE_OQ))));
}

/* bit = byte != 0 of a gathered dword (the LUT has SIFT_AGG_PAD readable bytes after it) */
K2 uint32
nonzero_byte(__m256i g)
{
	return ms32(_mm256_cmpeq_epi32(_mm256_and_si256(g, _mm256_set1_epi32(0xFF)), _mm256_setzero_si256()));
}

F2 void
lut_code_avx2(uint64 *out, const uint32 *code, int n, const uint8 *lut)
{
	AGK_BITS2(out, n, 8, ALL1, i, nonzero_byte(_mm256_i32gather_epi32((const int *) lut, LD(code + i), 1)),
			  nonzero_byte(_mm256_i32gather_epi32((const int *) lut, ld_n32(code + i, n - i), 1)));
}

/* Lanes outside the table are masked out of the gather (zero, so they read as "not set") */
K2 uint32
lut_i32_step(const uint8 *lut, __m256i x, __m256i vb, __m256i lim)
{
	__m256i		t = _mm256_sub_epi32(x, vb),
				in = _mm256_cmpeq_epi32(_mm256_min_epu32(t, lim), t);

	return nonzero_byte(_mm256_mask_i32gather_epi32(_mm256_setzero_si256(), (const int *) lut, t, in, 1));
}

F2 void
lut_i32_avx2(uint64 *out, const int32 *v, int n, int32 base, uint32 nlut, const uint8 *lut)
{
	const __m256i vb = _mm256_set1_epi32(base),
				lim = _mm256_set1_epi32(nlut - 1);

	if (nlut == 0)
	{
		memset(out, 0, AGK_NW(n) * 8);
		return;
	}
	AGK_BITS(out, n, 8, ALL1, i, lut_i32_step(lut, LD(v + i), vb, lim));
}

F2 void
bytes_mask_avx2(uint64 *out, const uint8 *b, int n)
{
	const __m256i z = _mm256_setzero_si256();

	AGK_BITS(out, n, 32, ALL1, i, _mm256_movemask_epi8(_mm256_cmpeq_epi8(LD(b + i), z)));
}

/* ---- bitmaps ---- */

F2 void
and_w_avx2(uint64 *dst, const uint64 *a, int nw)
{
	int			i = 0;

	for (; i + 4 <= nw; i += 4)
		ST(dst + i, _mm256_and_si256(LD(dst + i), LD(a + i)));
	for (; i < nw; i++)
		dst[i] &= a[i];
}

F2 void
or_w_avx2(uint64 *dst, const uint64 *a, int nw)
{
	int			i = 0;

	for (; i + 4 <= nw; i += 4)
		ST(dst + i, _mm256_or_si256(LD(dst + i), LD(a + i)));
	for (; i < nw; i++)
		dst[i] |= a[i];
}

F2 void
andnot_w_avx2(uint64 *dst, const uint64 *a, int nw)
{
	int			i = 0;

	for (; i + 4 <= nw; i += 4)
		ST(dst + i, _mm256_andnot_si256(LD(a + i), LD(dst + i)));
	for (; i < nw; i++)
		dst[i] &= ~a[i];
}

/* Nibble lookup popcount, bytes summed with PSADBW */
F2 int64
popcount_w_avx2(const uint64 *a, int nw)
{
	const __m256i tbl = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
										 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4),
				low = _mm256_set1_epi8(0x0F),
				z = _mm256_setzero_si256();
	__m256i		acc = z;
	int64		c;
	int			i = 0;

	for (; i + 4 <= nw; i += 4)
	{
		__m256i		x = LD(a + i),
					cnt = _mm256_add_epi8(_mm256_shuffle_epi8(tbl, _mm256_and_si256(x, low)),
										  _mm256_shuffle_epi8(tbl, _mm256_and_si256(_mm256_srli_epi16(x, 4), low)));

		acc = _mm256_add_epi64(acc, _mm256_sad_epu8(cnt, z));
	}
	c = hsum64(acc);
	for (; i < nw; i++)
		c += __builtin_popcountll(a[i]);
	return c;
}

/* bit e = bit parent[e] of rowmask: gather the dword that holds it, shift it down */
K2 uint32
row_bits(const uint64 *rowmask, __m256i p)
{
	__m256i		g = _mm256_i32gather_epi32((const int *) rowmask, _mm256_srli_epi32(p, 5), 4),
				s = _mm256_srlv_epi32(g, _mm256_and_si256(p, _mm256_set1_epi32(31)));

	return ms32(_mm256_slli_epi32(s, 31));
}

F2 void
gather_bits_avx2(uint64 *out, const uint64 *rowmask, const uint32 *parent, int m)
{
	AGK_BITS2(out, m, 8, 0, e, row_bits(rowmask, LD(parent + e)), row_bits(rowmask, ld_n32(parent + e, m - e)));
}

/* ---- selection vectors ---- */

F2 int
mask_sel_avx2(uint32 *sel, const uint64 *mask, int nw)
{
	int			cnt = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		m = mask[w];

		if (__builtin_popcountll(m) <= AGK_SEL_SPARSE)
		{
			for (; m; m &= m - 1)
				sel[cnt++] = w * 64 + __builtin_ctzll(m);
			continue;
		}
		AGK_UNROLL(8)
		for (int j = 0; j < 8; j++)
		{
			const uint32 b = (m >> (8 * j)) & 0xFF;

			ST(sel + cnt, _mm256_add_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *) lut_pos[b])),
										   _mm256_set1_epi32(w * 64 + 8 * j)));
			cnt += __builtin_popcount(b);
		}
	}
	return cnt;
}

/* ---- reductions ---- */

/* Sign-extends 8 selected int32 into two 4 x int64 accumulators */
K2 int64
sum_i32_t(const int32 *v, const uint64 *mask, int n, const bool masked)
{
	__m256i		a0 = _mm256_setzero_si256(),
				a1 = a0;

	for (int i = 0; i < n; i += 8)
	{
		__m256i		x = LD(v + i);

		if (masked)
			x = _mm256_and_si256(x, lanes32(BYTE(mask, i)));
		else if (i + 8 > n)
			x = _mm256_and_si256(x, lanes32(AGK_LANES(n - i, 8)));
		a0 = _mm256_add_epi64(a0, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(x)));
		a1 = _mm256_add_epi64(a1, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(x, 1)));
	}
	return hsum64(_mm256_add_epi64(a0, a1));
}

F2 int64
sum_i32_avx2(const int32 *v, const uint64 *mask, int n)
{
	return mask ? sum_i32_t(v, mask, n, true) : sum_i32_t(v, mask, n, false);
}

/* Unselected rows are ANDed to +0.0, which adds nothing to an accumulator (an accumulator is never -0.0) */
K2 double
sum_f64_t(const double *v, const uint64 *mask, int n, const bool masked)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	for (int i = 0; i < n; i += 8)
	{
		__m256d		x0 = LDPD(v + i),
					x1 = LDPD(v + i + 4);

		if (masked || i + 8 > n)
		{
			const uint32 b = masked ? BYTE(mask, i) : (uint32) AGK_LANES(n - i, 8);

			x0 = _mm256_and_pd(x0, _mm256_castsi256_pd(lanes64(b & 15)));
			x1 = _mm256_and_pd(x1, _mm256_castsi256_pd(lanes64(b >> 4)));
		}
		a0 = _mm256_add_pd(a0, x0);
		a1 = _mm256_add_pd(a1, x1);
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return agk_canon_nan(((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7])));
}

F2 double
sum_f64_avx2(const double *v, const uint64 *mask, int n)
{
	return mask ? sum_f64_t(v, mask, n, true) : sum_f64_t(v, mask, n, false);
}

K2 bool
minmax_i32_t(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n, const bool masked)
{
	const __m256i imax = _mm256_set1_epi32(INT32_MAX),
				imin = _mm256_set1_epi32(INT32_MIN);
	__m256i		lo = imax,
				hi = imin;
	uint32		any = 0;
	int32		l[8],
				h[8];
	int			i = 0;

	for (; !masked && i + 8 <= n; i += 8)
	{
		__m256i		x = LD(v + i);

		lo = _mm256_min_epi32(lo, x);
		hi = _mm256_max_epi32(hi, x);
	}
	for (; i < n; i += 8)
	{
		const uint32 b = masked ? BYTE(mask, i) : (uint32) AGK_LANES(n - i, 8);
		const __m256i x = LD(v + i),
					m = lanes32(b);

		any |= b;
		lo = _mm256_min_epi32(lo, _mm256_blendv_epi8(imax, x, m));
		hi = _mm256_max_epi32(hi, _mm256_blendv_epi8(imin, x, m));
	}
	if (!masked ? n == 0 : !any)
		return false;
	ST(l, lo);
	ST(h, hi);
	*mn = l[0];
	*mx = h[0];
	for (int j = 1; j < 8; j++)
	{
		*mn = Min(*mn, l[j]);
		*mx = Max(*mx, h[j]);
	}
	return true;
}

F2 bool
minmax_i32_avx2(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n)
{
	return mask ? minmax_i32_t(mn, mx, v, mask, n, true) : minmax_i32_t(mn, mx, v, mask, n, false);
}

/* min / max of 4 int64 lanes, the selected ones (m: all-ones lanes) */
K2 void
mm64_step(__m256i *lo, __m256i *hi, __m256i x, __m256i m)
{
	*lo = _mm256_blendv_epi8(*lo, x, _mm256_and_si256(_mm256_cmpgt_epi64(*lo, x), m));
	*hi = _mm256_blendv_epi8(*hi, x, _mm256_and_si256(_mm256_cmpgt_epi64(x, *hi), m));
}

K2 void
mm64_merge(__m256i *lo, __m256i *hi, __m256i lo2, __m256i hi2)
{
	*lo = _mm256_blendv_epi8(*lo, lo2, _mm256_cmpgt_epi64(*lo, lo2));
	*hi = _mm256_blendv_epi8(*hi, hi2, _mm256_cmpgt_epi64(hi2, *hi));
}

/* Four independent accumulator pairs: the compare + blend chain of one is too long for the loop to go at the load rate */
K2 bool
minmax_i64_t(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n, const bool masked)
{
	const __m256i all = _mm256_set1_epi64x(-1);
	__m256i		lo[4],
				hi[4];
	uint32		any = 0;
	int64		l[4],
				h[4];
	int			i = 0;

	for (int g = 0; g < 4; g++)
	{
		lo[g] = _mm256_set1_epi64x(INT64_MAX);
		hi[g] = _mm256_set1_epi64x(INT64_MIN);
	}
	for (; i + 16 <= n; i += 16)
	{
		const uint32 b = masked ? (uint32) (mask[i >> 6] >> (i & 63)) & 0xFFFF : 0xFFFF;

		any |= b;
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
			mm64_step(&lo[g], &hi[g], LD(v + i + 4 * g), masked ? lanes64((b >> (4 * g)) & 15) : all);
	}
	for (; i < n; i += 4)
	{
		const uint32 b = masked ? NIBBLE(mask, i) : (uint32) AGK_LANES(n - i, 4);

		any |= b;
		mm64_step(&lo[0], &hi[0], LD(v + i), lanes64(b));
	}
	if (!any)
		return false;
	for (int g = 1; g < 4; g++)
		mm64_merge(&lo[0], &hi[0], lo[g], hi[g]);
	ST(l, lo[0]);
	ST(h, hi[0]);
	*mn = Min(Min(l[0], l[1]), Min(l[2], l[3]));
	*mx = Max(Max(h[0], h[1]), Max(h[2], h[3]));
	return true;
}

F2 bool
minmax_i64_avx2(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n)
{
	return mask ? minmax_i64_t(mn, mx, v, mask, n, true) : minmax_i64_t(mn, mx, v, mask, n, false);
}

/*
 * MINPD / MAXPD return their second operand when the first is a NaN, so with the running value second a NaN never enters it;
 * NaN rows are noted on the side. The minimum is NaN only when every selected row is (the running minimum then never left
 * +Inf and the maximum never left -Inf: lo <= hi tells that a non-NaN row was seen), the maximum when any is. Unselected
 * lanes are replaced by +-Inf. Four accumulator pairs: the chain of one is too long for the loop to go at the load rate.
 */
K2 void
mmf_step(__m256d *lo, __m256d *hi, __m256d *anynan, __m256d x, __m256d sel)
{
	*anynan = _mm256_or_pd(*anynan, _mm256_and_pd(_mm256_cmp_pd(x, x, _CMP_UNORD_Q), sel));
	*lo = _mm256_min_pd(_mm256_blendv_pd(_mm256_set1_pd(INFINITY), x, sel), *lo);
	*hi = _mm256_max_pd(_mm256_blendv_pd(_mm256_set1_pd(-INFINITY), x, sel), *hi);
}

K2 bool
minmax_f64_t(double *mn, double *mx, const double *v, const uint64 *mask, int n, const bool masked)
{
	__m256d		lo[4],
				hi[4],
				anynan = _mm256_setzero_pd();
	uint32		any = 0;
	double		l[4],
				h[4];
	int			i = 0;

	for (int g = 0; g < 4; g++)
	{
		lo[g] = _mm256_set1_pd(INFINITY);
		hi[g] = _mm256_set1_pd(-INFINITY);
	}
	for (; !masked && i + 16 <= n; i += 16)
	{
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
		{
			const __m256d x = LDPD(v + i + 4 * g);

			anynan = _mm256_or_pd(anynan, _mm256_cmp_pd(x, x, _CMP_UNORD_Q));
			lo[g] = _mm256_min_pd(x, lo[g]);
			hi[g] = _mm256_max_pd(x, hi[g]);
		}
	}
	for (; i < n; i += 16)
	{
		const uint32 b = masked ? (uint32) (mask[i >> 6] >> (i & 63)) & 0xFFFF : (uint32) AGK_LANES(n - i, 16);

		any |= b;
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
			if (i + 4 * g < n)
				mmf_step(&lo[g], &hi[g], &anynan, LDPD(v + i + 4 * g), _mm256_castsi256_pd(lanes64((b >> (4 * g)) & 15)));
	}
	if (!masked ? n == 0 : !any)
		return false;
	for (int g = 1; g < 4; g++)
	{
		lo[0] = _mm256_min_pd(lo[g], lo[0]);
		hi[0] = _mm256_max_pd(hi[g], hi[0]);
	}
	_mm256_storeu_pd(l, lo[0]);
	_mm256_storeu_pd(h, hi[0]);
	{
		const double lov = Min(Min(l[0], l[1]), Min(l[2], l[3])),
					hiv = Max(Max(h[0], h[1]), Max(h[2], h[3]));

		*mn = agk_canon_minmax(lov <= hiv ? lov : __builtin_nan(""));
		*mx = agk_canon_minmax(_mm256_movemask_pd(anynan) ? __builtin_nan("") : hiv);
	}
	return true;
}

F2 bool
minmax_f64_avx2(double *mn, double *mx, const double *v, const uint64 *mask, int n)
{
	return mask ? minmax_f64_t(mn, mx, v, mask, n, true) : minmax_f64_t(mn, mx, v, mask, n, false);
}

/*
 * The selected values are packed to the front of a buffer behind the previous value (VPERMD with the position table), then
 * every value is compared with its predecessor in the buffer. Without a mask the rows are compared with themselves shifted
 * by one. The vector has at most SIFT_AGG_VEC rows.
 */
F2 int64
count_changes_avx2(const uint32 *v, const uint64 *mask, int n, uint32 *prev)
{
	uint32		buf[SIFT_AGG_VEC + 16];
	int64		cnt = 0;
	int			c = 0;

	if (!mask)
	{
		if (n == 0)
			return 0;
		cnt = v[0] != *prev;
		for (int i = 1; i < n; i += 8)
			cnt += __builtin_popcount(~ms32(_mm256_cmpeq_epi32(LD(v + i), LD(v + i - 1))) & AGK_LANES(n - i, 8));
		*prev = v[n - 1];
		return cnt;
	}
	buf[0] = *prev;
	for (int i = 0; i < n; i += 8)
	{
		const uint32 b = BYTE(mask, i);

		ST(buf + 1 + c, _mm256_permutevar8x32_epi32(LD(v + i), _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *) lut_pos[b]))));
		c += __builtin_popcount(b);
	}
	ST(buf + 1 + c, _mm256_setzero_si256());
	for (int j = 0; j < c; j += 8)
		cnt += __builtin_popcount(~ms32(_mm256_cmpeq_epi32(LD(buf + j), LD(buf + j + 1))) & AGK_LANES(c - j, 8));
	*prev = buf[c];
	return cnt;
}

#define AGK_INIT2(s)	.s = s##_avx2,

const SiftAggKernels sift_aggkern_avx2 = {
	AGK_INIT2(cmp_i32) AGK_INIT2(cmp_i64) AGK_INIT2(cmp_f64)
	AGK_INIT2(between_i32) AGK_INIT2(between_i64) AGK_INIT2(between_f64)
	AGK_INIT2(lut_code) AGK_INIT2(lut_i32) AGK_INIT2(bytes_mask)
	AGK_INIT2(and_w) AGK_INIT2(or_w) AGK_INIT2(andnot_w) AGK_INIT2(popcount_w) AGK_INIT2(gather_bits)
	AGK_INIT2(mask_sel)
	AGK_INIT2(sum_i32) AGK_INIT2(sum_f64) AGK_INIT2(minmax_i32) AGK_INIT2(minmax_i64) AGK_INIT2(minmax_f64)
	.gather_u32 = agk_gather_u32_scalar,
	.gather_u64 = agk_gather_u64_scalar,
	.gid_code = agk_gid_code_scalar,
	.gid_i32 = agk_gid_i32_scalar,
	.count_gid = agk_count_gid_scalar,
	.sum_gid_i32 = agk_sum_gid_i32_scalar,
	.sum_gid_f64 = agk_sum_gid_f64_scalar,
	AGK_INIT2(count_changes)
};

#endif							/* __x86_64__ */
