/*
 * agg/kern_avx2.c - AVX2 aggregation operators. Predicates compare 4 rows per instruction (doubles) and take the bits with
 * MOVMSK; lookups use VPGATHER (masked where a lane may not be touched); reductions keep several independent accumulators.
 * Results are bit-identical to the scalar table (agg/kern.c): float compares are mapped to unordered predicates so that
 * NaN is greater than everything. Slots that a per-group scatter (count_gid, sum_gid_*) or the gather of gid_code does not speed up are the scalar functions.
 */
#include "core/host.h"

#include "agg/kern_int.h"
#include "col/kern_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K2 static inline __attribute__((always_inline)) SIFT_TARGET_AVX2
#define F2 static SIFT_TARGET_AVX2

#define LD(p)		_mm256_loadu_si256((const __m256i *) (p))
#define ST(p, x)	_mm256_storeu_si256((__m256i *) (p), x)
#define LDPD(p)		_mm256_loadu_pd((const double *) (p))
#define ALL1		(~UINT64CONST(0))

#define R4(F, m)	F(m), F(m + 1), F(m + 2), F(m + 3)
#define R16(F, m)	R4(F, m), R4(F, m + 4), R4(F, m + 8), R4(F, m + 12)

K2 uint32
ms32(__m256i x)
{
	return _mm256_movemask_ps(_mm256_castsi256_ps(x));
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

#define NIBBLE(mask, i)	((uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & 0xF)

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

/* ---- predicates ---- */

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

/* bit = byte != 0 of a gathered dword (the LUT has SIFT_AGG_PAD readable bytes after it) */
K2 uint32
nonzero_byte(__m256i g)
{
	return ms32(_mm256_cmpeq_epi32(_mm256_and_si256(g, _mm256_set1_epi32(0xFF)), _mm256_setzero_si256()));
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

/* ---- group ids ---- */

F2 void
gid_i32_avx2(uint32 *gid, const int32 *v, int32 base, const uint32 *map, int n, uint32 mult)
{
	const __m256i vm = _mm256_set1_epi32(mult),
				vb = _mm256_set1_epi32(base);
	int			i = 0;

	for (; i + 8 <= n; i += 8)
		ST(gid + i, _mm256_add_epi32(LD(gid + i),
									 _mm256_mullo_epi32(_mm256_i32gather_epi32((const int *) map, _mm256_sub_epi32(LD(v + i), vb), 4), vm)));
	for (; i < n; i++)
		gid[i] += map[(uint32) v[i] - (uint32) base] * mult;
}

/* ---- reductions that keep the hand loop: the masked 50 % case measured 7 to 8 % slower in the shared body ---- */

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

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#endif
#define AKV_VB 32
#define AKV_NO_MINMAX_I64
#define AKV_NO_MINMAX_F64
#define AKV_BLEND(T, m, a, b) ((T) _mm256_blendv_epi8((__m256i) (b), (__m256i) (a), (__m256i) (m)))
#define AKV_LANES64(k) ((akv_s) lanes64(k))
#define AKV_MIN_akv_i32(a, b) ((akv_i32) _mm256_min_epi32((__m256i) (a), (__m256i) (b)))
#define AKV_MAX_akv_i32(a, b) ((akv_i32) _mm256_max_epi32((__m256i) (a), (__m256i) (b)))
#define AKV_MIN_akv_f64(a, b) ((akv_f64) _mm256_min_pd((__m256d) (a), (__m256d) (b)))
#define AKV_MAX_akv_f64(a, b) ((akv_f64) _mm256_max_pd((__m256d) (a), (__m256d) (b)))
#define AKV_WIDEN(a, b) _mm256_mul_epi32((__m256i) (a), (__m256i) (b))
#define AKV_BITS32(m) ((uint64) (uint32) _mm256_movemask_ps((__m256) (m)))
#define AKV_BITS64(m) ((uint64) (uint32) _mm256_movemask_pd((__m256d) (m)))
#define AKV_FN(x) x##_avx2
#include "agg/kern_vec.h"
#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif

#define AGK_INIT2(s)	.s = s##_avx2,

const SiftAggKernels sift_aggkern_avx2 = {
	AGK_INIT2(cmp_f64) AGK_INIT2(cmp2_i32) AGK_INIT2(cmp2_i64) AGK_INIT2(cmp2_f64) AGK_INIT2(lut_i32) AGK_INIT2(gather_bits)
	AGK_INIT2(sum_i32) AGK_INIT2(sum_i64) AGK_INIT2(absmax_f64) AGK_INIT2(absmax_i64) AGK_INIT2(minmax_i32) AGK_INIT2(minmax_i64) AGK_INIT2(minmax_f64)
	AGK_INIT2(lin_i64) AGK_INIT2(mulw_i64) AGK_INIT2(gid_i32)
	.gid_code = agk_gid_code_scalar,
	.count_gid = agk_count_gid_scalar,
	.sum_gid_i32 = agk_sum_gid_i32_scalar,
	.sum_gid_f64 = agk_sum_gid_f64_scalar,
	.sum_gid_i64 = agk_sum_gid_i64_scalar,
};

#endif							/* __x86_64__ */
