/*
 * Rejected or candidate designs measured by bench (not part of the library): the AVX-512 selections of the float sum.
 */
#include "harness.h"

/* The static helpers and the byte table of the AVX2 level; its table is renamed so that it does not clash */
#define sift_aggkern_avx2 alt_unused_avx2_table
#include "../../src/sift_aggkern_avx2.c"
#undef sift_aggkern_avx2

double		alt_sum_f64_a(const double *v, const uint64 *mask, int n);
double		alt_sum_f64_b(const double *v, const uint64 *mask, int n);
double		alt_sum_f64_c(const double *v, const uint64 *mask, int n);
double		alt_sum_f64_plain(const double *v, const uint64 *mask, int n);

SIFT_TARGET_AVX512 double
alt_sum_f64_a(const double *v, const uint64 *mask, int n)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	for (int i = 0; i < n; i += 8)
	{
		const uint32 k = (uint32) (mask[i >> 6] >> (i & 63)) & 0xFF;

		a0 = _mm256_add_pd(a0, _mm256_maskz_loadu_pd(k & 15, v + i));
		a1 = _mm256_add_pd(a1, _mm256_maskz_loadu_pd(k >> 4, v + i + 4));
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
}

SIFT_TARGET_AVX512 double
alt_sum_f64_b(const double *v, const uint64 *mask, int n)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	for (int i = 0; i < n; i += 8)
	{
		const uint32 k = (uint32) (mask[i >> 6] >> (i & 63)) & 0xFF;
		__m256d		x0 = _mm256_loadu_pd(v + i),
					x1 = _mm256_loadu_pd(v + i + 4);

		__asm__("" : "+x"(x0), "+x"(x1));
		a0 = _mm256_add_pd(a0, _mm256_maskz_mov_pd(k & 15, x0));
		a1 = _mm256_add_pd(a1, _mm256_maskz_mov_pd(k >> 4, x1));
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
}

SIFT_TARGET_AVX512 double
alt_sum_f64_c(const double *v, const uint64 *mask, int n)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	for (int i = 0; i < n; i += 8)
	{
		const uint32 b = (uint32) (mask[i >> 6] >> (i & 63)) & 0xFF;

		a0 = _mm256_add_pd(a0, _mm256_and_pd(_mm256_loadu_pd(v + i), _mm256_castsi256_pd(lanes64(b & 15))));
		a1 = _mm256_add_pd(a1, _mm256_and_pd(_mm256_loadu_pd(v + i + 4), _mm256_castsi256_pd(lanes64(b >> 4))));
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
}

/* unmasked: plain loads */
SIFT_TARGET_AVX512 double
alt_sum_f64_plain(const double *v, const uint64 *mask, int n)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	for (int i = 0; i + 8 <= n; i += 8)
	{
		a0 = _mm256_add_pd(a0, _mm256_loadu_pd(v + i));
		a1 = _mm256_add_pd(a1, _mm256_loadu_pd(v + i + 4));
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
}
