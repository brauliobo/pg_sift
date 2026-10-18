/*
 * sift_colcodec_avx2.c - AVX2 kernels of the chunk codec (x86-64-v3 function-level target, safe to link on any x86-64). Bit-identical to
 * the scalar table in sift_colcodec_kern.c; slots past the last value of rle_expand32 are unspecified (the caller allocates SIFT_COL_PAD).
 */
#if defined(__x86_64__)
#include "sift_host.h"

#include "sift_colcodec_int.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#endif
#include <immintrin.h>

#define CC_VB 32
#define CC_FN(x) cc_##x##_avx2
#include "sift_colcodec_vec.h"

#define LD256(p) _mm256_loadu_si256((const __m256i *) (p))
#define ST256(p, v) _mm256_storeu_si256((__m256i *) (p), (v))

/* unsigned x <= y per u32 lane */
CC_INLINE __m256i
le_epu32(__m256i x, __m256i y)
{
	return _mm256_cmpeq_epi32(_mm256_min_epu32(x, y), x);
}

static void
unpack_range32_avx2(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;
	uint32		row = 0;
	__m256i		vspan = _mm256_set1_epi32((int) span);

	for (uint32 w = 0; w < (rows + 1) / 2; w++)
		bits[w] = 0;
	if (W == 0)
	{
		if ((0u - lo) <= span)
			for (uint32 r = 0; r < rows; r++)
				bits[r / 2] |= (uint64) 0xFFFFFFFFu << ((r & 1) * 32);
		return;
	}
	while (row < rows)
	{
		uint32		R = rows - row > 32 ? 32 : rows - row;

		for (uint32 r = 0; r < R; r++, row++)
		{
			uint32		bp = r * W,
						m = 0;
			cc_vu32		v[CC_NV];

			cc_row_avx2(src + (size_t) (bp >> 5) * 32, bp & 31, W, mask, 0u - lo, v);
			for (int q = 0; q < CC_NV; q++)
			{
				__m256i		x = (__m256i) v[q];

				m |= (uint32) _mm256_movemask_ps(_mm256_castsi256_ps(le_epu32(x, vspan))) << (8 * q);
			}
			bits[row / 2] |= (uint64) m << ((row & 1) * 32);
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
	}
}

static void
prefix32_avx2(const uint32 *a, uint32 n, uint32 *dst, uint32 first, uint32 add)
{
	__m256i		vadd = _mm256_set1_epi32((int) add),
				carry = _mm256_set1_epi32((int) first),
				top = _mm256_set1_epi32(7);
	uint32		i = 0;

	if (n == 0)
		return;
	for (; i + 8 <= n; i += 8)
	{
		__m256i		x = _mm256_add_epi32(LD256(a + i), vadd),
					t;

		if (i == 0)
			x = _mm256_blend_epi32(x, _mm256_setzero_si256(), 1);
		x = _mm256_add_epi32(x, _mm256_slli_si256(x, 4));
		x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));
		t = _mm256_shuffle_epi32(x, 0xFF);
		x = _mm256_add_epi32(x, _mm256_permute2x128_si256(t, t, 0x08));
		x = _mm256_add_epi32(x, carry);
		ST256(dst + i, x);
		carry = _mm256_permutevar8x32_epi32(x, top);
	}
	if (i < n)
	{
		uint32		run = i ? dst[i - 1] : first;

		if (i == 0)
			dst[0] = run;
		for (uint32 k = i ? i : 1; k < n; k++)
		{
			run += a[k] + add;
			dst[k] = run;
		}
	}
}

static void
prefix64_avx2(const uint32 *a, uint32 n, int64 *dst, int64 first, int64 add)
{
	__m256i		vadd = _mm256_set1_epi64x((long long) add),
				carry = _mm256_set1_epi64x((long long) first);
	uint32		i = 0;

	if (n == 0)
		return;
	for (; i + 4 <= n; i += 4)
	{
		__m256i		x = _mm256_add_epi64(_mm256_cvtepu32_epi64(_mm_loadu_si128((const __m128i *) (a + i))), vadd),
					t;

		if (i == 0)
			x = _mm256_blend_epi32(x, _mm256_setzero_si256(), 0x03);
		x = _mm256_add_epi64(x, _mm256_slli_si256(x, 8));
		t = _mm256_shuffle_epi32(x, 0xEE);
		x = _mm256_add_epi64(x, _mm256_permute2x128_si256(t, t, 0x08));
		x = _mm256_add_epi64(x, carry);
		ST256(dst + i, x);
		carry = _mm256_permute4x64_epi64(x, 0xFF);
	}
	if (i < n)
	{
		uint64		run = i ? (uint64) dst[i - 1] : (uint64) first;

		if (i == 0)
			dst[0] = (int64) run;
		for (uint32 k = i ? i : 1; k < n; k++)
		{
			run += (uint64) a[k] + (uint64) add;
			dst[k] = (int64) run;
		}
	}
}

static uint32
max32_avx2(const uint32 *a, uint32 n)
{
	__m256i		m = _mm256_setzero_si256();
	uint32		i = 0,
				r;
	__m128i		h;

	for (; i + 8 <= n; i += 8)
		m = _mm256_max_epu32(m, LD256(a + i));
	h = _mm_max_epu32(_mm256_castsi256_si128(m), _mm256_extracti128_si256(m, 1));
	h = _mm_max_epu32(h, _mm_shuffle_epi32(h, 0x4E));
	h = _mm_max_epu32(h, _mm_shuffle_epi32(h, 0xB1));
	r = (uint32) _mm_cvtsi128_si32(h);
	for (; i < n; i++)
		r = a[i] > r ? a[i] : r;
	return r;
}

/* key of four doubles: bits ^ ((bits >> 63) >> 1) with an arithmetic shift, from a compare against zero */
CC_INLINE __m256i
keys4(__m256d x)
{
	__m256i		b = _mm256_castpd_si256(x),
				neg = _mm256_cmpgt_epi64(_mm256_setzero_si256(), b);

	return _mm256_xor_si256(b, _mm256_srli_epi64(neg, 1));
}

static void
alp_keys_f8_avx2(int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div)
{
	/* (double) (dbase + u) = (double) (int32) (u ^ 2^31) + (2^31 + dbase), both exact: the encoder keeps |digits| below 2^52 */
	__m256d		off = _mm256_set1_pd(2147483648.0 + (double) dbase),
				vd = _mm256_set1_pd(div);
	__m128i		flip = _mm_set1_epi32((int) 0x80000000u);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m128i		x = _mm_xor_si128(_mm_loadu_si128((const __m128i *) (u + i)), flip);
		__m256d		d = _mm256_add_pd(_mm256_cvtepi32_pd(x), off);

		ST256(dst + i, keys4(_mm256_div_pd(d, vd)));
	}
	sift_cc_kernels_scalar.alp_keys_f8(dst + i, u + i, n - i, dbase, div);
}

static void
alp_keys_f4_avx2(int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	__m256i		vb = _mm256_set1_epi32((int) (uint32) dbase);
	__m256		vd = _mm256_set1_ps(div);
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m256		x = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(LD256(u + i), vb)), vd);

		ST256(dst + i, keys4(_mm256_cvtps_pd(_mm256_castps256_ps128(x))));
		ST256(dst + i + 4, keys4(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1))));
	}
	sift_cc_kernels_scalar.alp_keys_f4(dst + i, u + i, n - i, dbase, div);
}

static void
rle_expand32_avx2(const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst)
{
	for (uint32 r = 0; r < nruns; r++)
	{
		__m256i		v = _mm256_set1_epi32((int) vals[r]);
		uint32		len = lens[r];

		for (uint32 k = 0; k < len; k += 8)
			ST256(dst + k, v);
		dst += len;
	}
}

const SiftCcKernels sift_cc_kernels_avx2 = {
	.unpack32 = cc_unpack32_avx2,
	.unpack_range32 = unpack_range32_avx2,
	.prefix32 = prefix32_avx2,
	.prefix64 = prefix64_avx2,
	.max32 = max32_avx2,
	.alp_keys_f8 = alp_keys_f8_avx2,
	.alp_keys_f4 = alp_keys_f4_avx2,
	.rle_expand32 = rle_expand32_avx2,
};

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#endif							/* __x86_64__ */
