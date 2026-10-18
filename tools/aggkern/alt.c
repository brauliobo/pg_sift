/*
 * Rejected or candidate designs measured by bench (not part of the library): selection vectors from VPCOMPRESSB (one compress per
 * mask word, widened and stored in four steps), and the AVX2 byte-table selection with a scalar path for sparse words.
 */
#include "harness.h"

/* The static helpers and the byte table of the AVX2 level; its table is renamed so that it does not clash */
#define sift_aggkern_avx2 alt_unused_avx2_table
#include "../../src/sift_aggkern_avx2.c"
#undef sift_aggkern_avx2

int			alt_mask_sel_compressb(uint32 *sel, const uint64 *mask, int nw);
int			alt_mask_sel_avx2_sparse(uint32 *sel, const uint64 *mask, int nw, int thr);
int			alt_mask_sel_avx512_sparse(uint32 *sel, const uint64 *mask, int nw, int thr);
double		alt_sum_f64_a(const double *v, const uint64 *mask, int n);
double		alt_sum_f64_b(const double *v, const uint64 *mask, int n);
double		alt_sum_f64_c(const double *v, const uint64 *mask, int n);
double		alt_sum_f64_plain(const double *v, const uint64 *mask, int n);


SIFT_TARGET_AVX512 int
alt_mask_sel_compressb(uint32 *sel, const uint64 *mask, int nw)
{
	const __m512i iota = _mm512_set_epi64(0x3F3E3D3C3B3A3938, 0x3736353433323130, 0x2F2E2D2C2B2A2928, 0x2726252423222120,
										  0x1F1E1D1C1B1A1918, 0x1716151413121110, 0x0F0E0D0C0B0A0908, 0x0706050403020100);
	int			cnt = 0;

	for (int w = 0; w < nw; w++)
	{
		const uint64 m = mask[w];
		__m512i		pos,
					base;

		if (!m)
			continue;
		pos = _mm512_maskz_compress_epi8(m, iota);
		base = _mm512_set1_epi32(w * 64);
		_mm512_storeu_si512(sel + cnt, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_castsi512_si128(pos)), base));
		_mm512_storeu_si512(sel + cnt + 16, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(pos, 1)), base));
		_mm512_storeu_si512(sel + cnt + 32, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(pos, 2)), base));
		_mm512_storeu_si512(sel + cnt + 48, _mm512_add_epi32(_mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(pos, 3)), base));
		cnt += __builtin_popcountll(m);
	}
	return cnt;
}

/* Words with at most thr bits take a scalar loop, the others the byte table of the library */
SIFT_TARGET_AVX2 int
alt_mask_sel_avx2_sparse(uint32 *sel, const uint64 *mask, int nw, int thr)
{
	int			cnt = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		m = mask[w];

		if (!m)
			continue;
		if (__builtin_popcountll(m) <= thr)
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

/* The same cutover over the VPCOMPRESSD selection of the library */
SIFT_TARGET_AVX512 int
alt_mask_sel_avx512_sparse(uint32 *sel, const uint64 *mask, int nw, int thr)
{
	const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
				step = _mm512_set1_epi32(16);
	int			cnt = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		m = mask[w];
		__m512i		x = _mm512_add_epi32(iota, _mm512_set1_epi32(w * 64));

		if (!m)
			continue;
		if (__builtin_popcountll(m) <= thr)
		{
			for (; m; m &= m - 1)
				sel[cnt++] = w * 64 + __builtin_ctzll(m);
			continue;
		}
		AGK_UNROLL(4)
		for (int j = 0; j < 4; j++)
		{
			const __mmask16 k = (__mmask16) (m >> (16 * j));

			_mm512_storeu_si512(sel + cnt, _mm512_maskz_compress_epi32(k, x));
			cnt += __builtin_popcount(k);
			x = _mm512_add_epi32(x, step);
		}
	}
	return cnt;
}

/* sum_f64 with a mask: variants of the selection of the lanes (A: zeroing masked load, B: load + zeroing move, C: load + AND with a lane table) */
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
