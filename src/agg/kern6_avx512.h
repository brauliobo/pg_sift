/*
 * agg/kern6_avx512.h - what the AVX-512 files of the 2048-row operators share (agg/kern6_avx512*.c): the load and store shorthands, the
 * hash step with VPMULLQ, the loads of eight rows through a selection vector (a gather), the float8 key of eight doubles, and the
 * 128-bit order on mask registers. The grouped accumulation is a gather, an add and a scatter of eight cells, which is exact only when
 * the eight groups differ: VPCONFLICTD says so, and a window with a repeated group runs its rows in order (agg/kern6_avx512_agg.c).
 */
#ifndef SIFT_AGGKERN6_AVX512_H
#define SIFT_AGGKERN6_AVX512_H

#include "agg/kern6_int.h"

#if defined(__x86_64__)

#include <immintrin.h>

#define K5 static inline __attribute__((always_inline)) SIFT_TARGET_AVX512
#define F5 static SIFT_TARGET_AVX512
#define X5 SIFT_TARGET_AVX512

#define LD(p)		_mm512_loadu_si512((const void *) (p))
#define ST(p, x)	_mm512_storeu_si512((void *) (p), x)
#define LDPD(p)		_mm512_loadu_pd((const void *) (p))
#define LO32		_mm512_set1_epi64(0xFFFFFFFF)
#define SET64(x)	_mm512_set1_epi64((int64) (x))
#define SET32(x)	_mm512_set1_epi32((int) (x))

#define BITS_AT(mask, i, m)	((uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & (m))
#define ROWBIT(mask, i)		AK6_ROWBIT(mask, i)

K5 __m512i
hash1(__m512i x)
{
	x = _mm512_mullo_epi64(_mm512_xor_si512(x, _mm512_srli_epi64(x, 32)), _mm512_set1_epi64((int64) UINT64CONST(0x9E3779B97F4A7C15)));
	x = _mm512_mullo_epi64(_mm512_xor_si512(x, _mm512_srli_epi64(x, 29)), _mm512_set1_epi64((int64) UINT64CONST(0xBF58476D1CE4E5B9)));
	return _mm512_xor_si512(x, _mm512_srli_epi64(x, 32));
}

K5 __m512i
hash_step(__m512i h, __m512i x, bool mix)
{
	return hash1(mix ? _mm512_xor_si512(_mm512_rol_epi64(h, 5), x) : x);
}

/* The indexes sel[i..i+8) as 32-bit lanes */
K5 __m256i
sel8(const uint16 *sel)
{
	return _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *) sel));
}

/* Four int64 of the active rows s[0..4) (a gather is slower than loads on the cores that have it microcoded) */
K5 __m256i
set64x4(const int64 *v, const uint16 *s)
{
	return _mm256_set_epi64x(v[s[3]], v[s[2]], v[s[1]], v[s[0]]);
}

/* Eight int64 of the active rows i..i+8 */
K5 __m512i
ld64x8(const int64 *v, const uint16 *sel, int i)
{
	return sel ? _mm512_inserti64x4(_mm512_castsi256_si512(set64x4(v, sel + i)), set64x4(v, sel + i + 4), 1) : LD(v + i);
}

/* Eight int32 of the active rows i..i+8, widened */
K5 __m512i
ld32x8(const int32 *v, const uint16 *sel, int i)
{
	return _mm512_cvtepi32_epi64(sel ? _mm256_set_epi32(v[sel[i + 7]], v[sel[i + 6]], v[sel[i + 5]], v[sel[i + 4]], v[sel[i + 3]], v[sel[i + 2]],
														v[sel[i + 1]], v[sel[i]])
								 : _mm256_loadu_si256((const __m256i *) (v + i)));
}

/* Four 16 byte values of the active rows s[0..4) as one register */
K5 __m512i
set128x4(const void *v, const uint16 *s)
{
	const char *p = v;

	return _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set_m128i(_mm_loadu_si128((const __m128i *) (p + 16 * (size_t) s[1])),
																	  _mm_loadu_si128((const __m128i *) (p + 16 * (size_t) s[0])))),
							  _mm256_set_m128i(_mm_loadu_si128((const __m128i *) (p + 16 * (size_t) s[3])),
											   _mm_loadu_si128((const __m128i *) (p + 16 * (size_t) s[2]))), 1);
}

/* The low and the high halves of eight 16 byte values of the active rows i..i+8 */
K5 void
ld128x8(const void *v, const uint16 *sel, int i, __m512i *lo, __m512i *hi)
{
	const __m512i a = sel ? set128x4(v, sel + i) : LD((const char *) v + 16 * i),
				b = sel ? set128x4(v, sel + i + 4) : LD((const char *) v + 16 * i + 64),
				ilo = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14),
				ihi = _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15);

	*lo = _mm512_permutex2var_epi64(a, ilo, b);
	*hi = _mm512_permutex2var_epi64(a, ihi, b);
}

/* The float8 key (ak6_f8_key) of eight doubles: -0.0 as 0, every NaN the quiet NaN, then the low 63 bits flipped for a negative number */
K5 __m512i
f8key8(__m512d x)
{
	const __mmask8 nan = _mm512_cmp_pd_mask(x, x, _CMP_UNORD_Q),
				zero = _mm512_cmp_pd_mask(x, _mm512_setzero_pd(), _CMP_EQ_OQ);
	__m512i		b = _mm512_maskz_mov_epi64((__mmask8) ~zero, _mm512_castpd_si512(x));

	b = _mm512_mask_mov_epi64(b, nan, SET64(AK6_F8_NAN_BITS));
	return _mm512_xor_si512(b, _mm512_and_si512(_mm512_srai_epi64(b, 63), SET64(AK6_F8_FLIP)));
}

K5 __m512d
f8unkey8(__m512i k)
{
	return _mm512_castsi512_pd(_mm512_xor_si512(k, _mm512_and_si512(_mm512_srai_epi64(k, 63), SET64(AK6_F8_FLIP))));
}

/* The pairs (low, high) of 128-bit values of a and b: the lanes of both halves of a pair where a < b (high signed, then low unsigned) */
K5 __mmask8
lt128_8(__m512i a, __m512i b)
{
	const __mmask8 mu = _mm512_cmplt_epu64_mask(a, b),
				ms = _mm512_cmplt_epi64_mask(a, b),
				me = _mm512_cmpeq_epi64_mask(a, b),
				lt = ((ms >> 1) & 0x55) | (((me >> 1) & 0x55) & (mu & 0x55));

	return (__mmask8) (lt | (lt << 1));
}

/* The slots of the level that its other files define (declared for them and for the table) */
#define AK6_OTHER_AVX512(X) \
	X(cmp_i32) X(cmp_i64) X(cmp_f64) X(cmp_i128) X(cmp2_i32) X(cmp2_i64) X(cmp2_f64) X(lut_u32) X(in_set_i64) X(cmp_strview_eq) \
	X(stream_run_ne_i64) X(minmax_i32) X(minmax_i64) X(minmax_i128) X(minmax_f64)
/*
 * The grouped slots by VPCONFLICTD, gather and scatter (agg/kern6_avx512_agg.c). They are exact, and not the slots of the table of the level: on the
 * AMD cores measured (Zen 5) a gather and a scatter of eight cells cost more than eight loop iterations, 1.5 to 3 times, so the table of the level has
 * the loop of agg/kern6_gid.h and these are the table sift_aggkern6_avx512_conflict, for a core whose scatter is fast.
 */
#define AK6_CONFLICT_AVX512(X) \
	X(count_gid) X(sum_gid_i32) X(sum_gid_i64) X(sum_gid_i64_to_i128) X(sum_gid_i128) X(sum_gid_f64_fast) X(minmax_gid_i64) X(minmax_gid_f64)
#define AK6_DECL5(s)	AK6_DECLARE_AT(s, avx512)
AK6_OTHER_AVX512(AK6_DECL5)
AK6_CONFLICT_AVX512(AK6_DECL5)

#endif							/* __x86_64__ */

#endif							/* SIFT_AGGKERN6_AVX512_H */
