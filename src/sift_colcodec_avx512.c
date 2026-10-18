/*
 * sift_colcodec_avx512.c - AVX-512 kernels of the chunk codec (F, BW, VL, DQ + the options of SIFT_TARGET_AVX512). Bit-identical to the
 * scalar table in sift_colcodec_kern.c; slots past the last value of rle_expand32 are unspecified (the caller allocates SIFT_COL_PAD).
 */
#if defined(__x86_64__)
#include "sift_host.h"

#include "sift_colcodec_int.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt")
#endif
#include <immintrin.h>

#define CC_VB 64
#define CC_FN(x) cc_##x##_avx512
#include "sift_colcodec_vec.h"

#define LD512(p) _mm512_loadu_si512((const void *) (p))
#define ST512(p, v) _mm512_storeu_si512((void *) (p), (v))

static void
unpack_range32_avx512(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;
	uint32		row = 0;
	__m512i		vspan = _mm512_set1_epi32((int) span);

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

			cc_row_avx512(src + (size_t) (bp >> 5) * 32, bp & 31, W, mask, 0u - lo, v);
			for (int q = 0; q < CC_NV; q++)
				m |= (uint32) _mm512_cmple_epu32_mask((__m512i) v[q], vspan) << (16 * q);
			bits[row / 2] |= (uint64) m << ((row & 1) * 32);
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
	}
}

static void
prefix32_avx512(const uint32 *a, uint32 n, uint32 *dst, uint32 first, uint32 add)
{
	__m512i		vadd = _mm512_set1_epi32((int) add),
				carry = _mm512_set1_epi32((int) first),
				top = _mm512_set1_epi32(15),
				zero = _mm512_setzero_si512();
	uint32		i = 0;

	if (n == 0)
		return;
	for (; i + 16 <= n; i += 16)
	{
		__m512i		x = _mm512_add_epi32(LD512(a + i), vadd);

		if (i == 0)
			x = _mm512_mask_blend_epi32(1, x, zero);
		x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 15));
		x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 14));
		x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 12));
		x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 8));
		x = _mm512_add_epi32(x, carry);
		ST512(dst + i, x);
		carry = _mm512_permutexvar_epi32(top, x);
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
prefix64_avx512(const uint32 *a, uint32 n, int64 *dst, int64 first, int64 add)
{
	__m512i		vadd = _mm512_set1_epi64((long long) add),
				carry = _mm512_set1_epi64((long long) first),
				top = _mm512_set1_epi64(7),
				zero = _mm512_setzero_si512();
	uint32		i = 0;

	if (n == 0)
		return;
	for (; i + 8 <= n; i += 8)
	{
		__m512i		x = _mm512_add_epi64(_mm512_cvtepu32_epi64(_mm256_loadu_si256((const __m256i *) (a + i))), vadd);

		if (i == 0)
			x = _mm512_mask_blend_epi64(1, x, zero);
		x = _mm512_add_epi64(x, _mm512_alignr_epi64(x, zero, 7));
		x = _mm512_add_epi64(x, _mm512_alignr_epi64(x, zero, 6));
		x = _mm512_add_epi64(x, _mm512_alignr_epi64(x, zero, 4));
		x = _mm512_add_epi64(x, carry);
		ST512(dst + i, x);
		carry = _mm512_permutexvar_epi64(top, x);
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
max32_avx512(const uint32 *a, uint32 n)
{
	__m512i		m = _mm512_setzero_si512();
	uint32		i = 0,
				r;

	for (; i + 16 <= n; i += 16)
		m = _mm512_max_epu32(m, LD512(a + i));
	r = _mm512_reduce_max_epu32(m);
	for (; i < n; i++)
		r = a[i] > r ? a[i] : r;
	return r;
}

/* key of eight doubles */
CC_INLINE __m512i
keys8(__m512d x)
{
	__m512i		b = _mm512_castpd_si512(x);

	return _mm512_xor_si512(b, _mm512_srli_epi64(_mm512_srai_epi64(b, 63), 1));
}

static void
alp_keys_f8_avx512(int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div)
{
	__m512d		off = _mm512_set1_pd(2147483648.0 + (double) dbase),
				vd = _mm512_set1_pd(div);
	__m256i		flip = _mm256_set1_epi32((int) 0x80000000u);
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m256i		x = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *) (u + i)), flip);
		__m512d		d = _mm512_add_pd(_mm512_cvtepi32_pd(x), off);

		ST512(dst + i, keys8(_mm512_div_pd(d, vd)));
	}
	sift_cc_kernels_scalar.alp_keys_f8(dst + i, u + i, n - i, dbase, div);
}

static void
alp_keys_f4_avx512(int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	__m512i		vb = _mm512_set1_epi32((int) (uint32) dbase);
	__m512		vd = _mm512_set1_ps(div);
	uint32		i = 0;

	for (; i + 16 <= n; i += 16)
	{
		__m512		x = _mm512_div_ps(_mm512_cvtepi32_ps(_mm512_add_epi32(LD512(u + i), vb)), vd);

		ST512(dst + i, keys8(_mm512_cvtps_pd(_mm512_castps512_ps256(x))));
		ST512(dst + i + 8, keys8(_mm512_cvtps_pd(_mm512_extractf32x8_ps(x, 1))));
	}
	sift_cc_kernels_scalar.alp_keys_f4(dst + i, u + i, n - i, dbase, div);
}

static void
rle_expand32_avx512(const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst)
{
	for (uint32 r = 0; r < nruns; r++)
	{
		__m512i		v = _mm512_set1_epi32((int) vals[r]);
		uint32		len = lens[r];

		for (uint32 k = 0; k < len; k += 16)
			ST512(dst + k, v);
		dst += len;
	}
}

const SiftCcKernels sift_cc_kernels_avx512 = {
	.unpack32 = cc_unpack32_avx512,
	.unpack_range32 = unpack_range32_avx512,
	.prefix32 = prefix32_avx512,
	.prefix64 = prefix64_avx512,
	.max32 = max32_avx512,
	.alp_keys_f8 = alp_keys_f8_avx512,
	.alp_keys_f4 = alp_keys_f4_avx512,
	.rle_expand32 = rle_expand32_avx512,
};

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#endif							/* __x86_64__ */
