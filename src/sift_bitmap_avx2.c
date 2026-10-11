/* AVX2 + BMI1/2 + POPCNT kernels (x86-64-v3); function-level target, safe to link on any x86-64. */
#if defined(__x86_64__)
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#endif
#include <immintrin.h>

#define KFN(x) x##_avx2
#define or_wide_avx2 sift_or_wide_gen
#define andnot_wide_avx2 sift_andnot_wide_gen
#include "sift_bitmap_kern.h"

/* w >= 16, multiple of 8: one 32-byte, 16-byte and 8-byte step as needed */
SIFT_INLINE int and_wide_avx2(uint8_t *d, const uint8_t *a, const uint8_t *b, uint32_t w)
{
	__m256i acc = _mm256_setzero_si256();
	uint32_t q = 0;
	for (; q + 32 <= w; q += 32) {
		__m256i x = _mm256_and_si256(_mm256_loadu_si256((const __m256i *)(a + q)), _mm256_loadu_si256((const __m256i *)(b + q)));
		_mm256_storeu_si256((__m256i *)(d + q), x);
		acc = _mm256_or_si256(acc, x);
	}
	if (q + 16 <= w) {
		__m128i x = _mm_and_si128(_mm_loadu_si128((const __m128i *)(a + q)), _mm_loadu_si128((const __m128i *)(b + q)));
		_mm_storeu_si128((__m128i *)(d + q), x);
		acc = _mm256_or_si256(acc, _mm256_castsi128_si256(x));
		q += 16;
	}
	uint64_t t = 0;
	if (q < w) sift_st8(d + q, t = sift_ld8(a + q) & sift_ld8(b + q));
	return !_mm256_testz_si256(acc, acc) || t;
}

SIFT_INLINE uint64_t cnt_wide_avx2(const uint8_t *a, const uint8_t *b, uint32_t w)
{
	uint64_t c = 0;
	for (uint32_t q = 0; q < w; q += 8) c += (uint64_t)_mm_popcnt_u64(sift_ld8(a + q) & sift_ld8(b + q));
	return c;
}

#define SIFT_KERN_TMPL
#include "sift_bitmap_kern.h"

/* Mula's nibble-LUT popcount for regions of 64+ bytes */
static uint64_t popcount_avx2_lut(const uint8_t *p, size_t n)
{
	const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
	const __m256i low = _mm256_set1_epi8(0x0f);
	__m256i acc = _mm256_setzero_si256();
	size_t i = 0;
	for (; i + 32 <= n; i += 32) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(p + i));
		__m256i c = _mm256_add_epi8(_mm256_shuffle_epi8(lut, _mm256_and_si256(v, low)),
					    _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), low)));
		acc = _mm256_add_epi64(acc, _mm256_sad_epu8(c, _mm256_setzero_si256()));
	}
	uint64_t c = (uint64_t)(_mm256_extract_epi64(acc, 0) + _mm256_extract_epi64(acc, 1) + _mm256_extract_epi64(acc, 2) + _mm256_extract_epi64(acc, 3));
	return c + popcount_avx2(p + i, n - i);
}

static uint64_t popcount_avx2_dispatch(const uint8_t *p, size_t n) { return n >= 64 ? popcount_avx2_lut(p, n) : popcount_avx2(p, n); }

const sift_kernels sift_kern_avx2 = { popcount_avx2_dispatch, and2_avx2, or2_avx2, andnot_avx2, and_count_avx2,
				      count_mask_avx2, tids_avx2, ranks_avx2 };

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#else
#include "sift_bitmap_kern.h"
const sift_kernels sift_kern_avx2 = { 0 };
#endif
