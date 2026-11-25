/*
 * agg/kern/avx2.h - what the AVX2 files of the 2048-row operators share (agg/kern6_avx2*.c): the load and store shorthands, the lane
 * masks of a bitmap word, the 64-bit multiply built from 32-bit products, the hash step, and the loads of four rows through a selection
 * vector (a gather: AVX2 has no scatter, so the grouped accumulation is in agg/kern/gid.h).
 */
#ifndef SIFT_AGGKERN6_AVX2_H
#define SIFT_AGGKERN6_AVX2_H

#include "agg/kern/int.h"

#if defined(__x86_64__)

#include <immintrin.h>

#define K2 static inline __attribute__((always_inline)) SIFT_TARGET_AVX2
#define F2 static SIFT_TARGET_AVX2
#define X2 SIFT_TARGET_AVX2

#define LD(p)		_mm256_loadu_si256((const __m256i *) (p))
#define ST(p, x)	_mm256_storeu_si256((__m256i *) (p), x)
#define LDPD(p)		_mm256_loadu_pd((const double *) (p))
#define LO32		_mm256_set1_epi64x(0xFFFFFFFF)
#define SET64(x)	_mm256_set1_epi64x((int64) (x))
#define SET32(x)	_mm256_set1_epi32((int) (x))
#define SIGN64		((int64) INT64_MIN)

#define BITS_AT(mask, i, m)	((uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & (m))

#define R4(F, m)	F(m), F(m + 1), F(m + 2), F(m + 3)
#define R16(F, m)	R4(F, m), R4(F, m + 4), R4(F, m + 8), R4(F, m + 12)

/* All-ones 64-bit lanes for the set bits of k (4 bits) */
#define LM(n)		{-(int64) ((n) & 1), -(int64) (((n) >> 1) & 1), -(int64) (((n) >> 2) & 1), -(int64) (((n) >> 3) & 1)}
static const int64 lane_tab[16][4] __attribute__((aligned(32))) = {R16(LM, 0)};

K2 __m256i
lanes64(uint32 k)
{
	return _mm256_load_si256((const __m256i *) lane_tab[k]);
}

/* All-ones 32-bit lanes for the set bits of k (8 bits) */
K2 __m256i
lanes32(uint32 k)
{
	const __m256i bit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);

	return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(k), bit), bit);
}

/* The low 64 bits of a * b per lane */
K2 __m256i
mul64(__m256i a, __m256i b)
{
	__m256i		cross = _mm256_add_epi64(_mm256_mul_epu32(_mm256_srli_epi64(a, 32), b),
										 _mm256_mul_epu32(a, _mm256_srli_epi64(b, 32)));

	return _mm256_add_epi64(_mm256_mul_epu32(a, b), _mm256_slli_epi64(cross, 32));
}

K2 __m256i
hash1(__m256i x)
{
	x = mul64(_mm256_xor_si256(x, _mm256_srli_epi64(x, 32)), _mm256_set1_epi64x((int64) UINT64CONST(0x9E3779B97F4A7C15)));
	x = mul64(_mm256_xor_si256(x, _mm256_srli_epi64(x, 29)), _mm256_set1_epi64x((int64) UINT64CONST(0xBF58476D1CE4E5B9)));
	return _mm256_xor_si256(x, _mm256_srli_epi64(x, 32));
}

K2 __m256i
hash_step(__m256i h, __m256i x, bool mix)
{
	if (mix)
		x = _mm256_xor_si256(_mm256_or_si256(_mm256_slli_epi64(h, 5), _mm256_srli_epi64(h, 59)), x);
	return hash1(x);
}

/* Four int64 of the active rows i..i+4 (a gather is slower than four loads on the cores that have it microcoded) */
K2 __m256i
ld64x4(const int64 *v, const uint16 *sel, int i)
{
	return sel ? _mm256_set_epi64x(v[sel[i + 3]], v[sel[i + 2]], v[sel[i + 1]], v[sel[i]]) : LD(v + i);
}

/* Four int32 of the active rows i..i+4, widened */
K2 __m256i
ld32x4(const int32 *v, const uint16 *sel, int i)
{
	return _mm256_cvtepi32_epi64(sel ? _mm_set_epi32(v[sel[i + 3]], v[sel[i + 2]], v[sel[i + 1]], v[sel[i]])
								 : _mm_loadu_si128((const __m128i *) (v + i)));
}

/* The low and the high halves of four 16 byte values of the active rows i..i+4 */
K2 void
ld128x4(const void *v, const uint16 *sel, int i, __m256i *lo, __m256i *hi)
{
	const char *p = v;
	__m256i		a,
				b;

	if (sel)
	{
		a = _mm256_set_m128i(_mm_loadu_si128((const __m128i *) (p + 16 * (size_t) sel[i + 1])),
							 _mm_loadu_si128((const __m128i *) (p + 16 * (size_t) sel[i])));
		b = _mm256_set_m128i(_mm_loadu_si128((const __m128i *) (p + 16 * (size_t) sel[i + 3])),
							 _mm_loadu_si128((const __m128i *) (p + 16 * (size_t) sel[i + 2])));
	}
	else
	{
		a = LD(p + 16 * i);
		b = LD(p + 16 * i + 32);
	}
	*lo = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(a, b), 0xD8);
	*hi = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(a, b), 0xD8);
}

/* The float8 key (ak6_f8_key) of four doubles: -0.0 as 0, every NaN the quiet NaN, then the low 63 bits flipped for a negative number */
K2 __m256i
f8key4(__m256d x)
{
	const __m256d nan = _mm256_cmp_pd(x, x, _CMP_UNORD_Q),
				zero = _mm256_cmp_pd(x, _mm256_setzero_pd(), _CMP_EQ_OQ);
	__m256i		b = _mm256_andnot_si256(_mm256_castpd_si256(zero), _mm256_castpd_si256(x));

	b = _mm256_blendv_epi8(b, SET64(AK6_F8_NAN_BITS), _mm256_castpd_si256(nan));
	return _mm256_xor_si256(b, _mm256_and_si256(_mm256_cmpgt_epi64(_mm256_setzero_si256(), b), SET64(AK6_F8_FLIP)));
}

/* The popcount of four words */
K2 uint32
pc4(__m256i x)
{
	return (uint32) (_mm_popcnt_u64(_mm256_extract_epi64(x, 0)) + _mm_popcnt_u64(_mm256_extract_epi64(x, 1)) +
					 _mm_popcnt_u64(_mm256_extract_epi64(x, 2)) + _mm_popcnt_u64(_mm256_extract_epi64(x, 3)));
}

/* The slots of the level that its other files define (declared for them and for the table) */
#define AK6_OTHER_AVX2(X) \
	X(cmp_i32) X(cmp_i64) X(cmp_f64) X(cmp_i128) X(cmp2_i32) X(cmp2_i64) X(cmp2_f64) X(lut_u32) X(in_set_i64) X(cmp_strview_eq) \
	X(stream_run_ne_i64) X(minmax_i32) X(minmax_i64) X(minmax_i128) X(minmax_f64)
#define AK6_DECL2(s)	AK6_DECLARE_AT(s, avx2)
AK6_OTHER_AVX2(AK6_DECL2)

#endif							/* __x86_64__ */

#endif							/* SIFT_AGGKERN6_AVX2_H */
