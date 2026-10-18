/*
 * sift_colcodec_avx2.c - AVX2 kernels of the chunk codec (x86-64-v3 function-level target, safe to link on any x86-64). Bit-identical to
 * the scalar table in sift_colcodec_kern.c; slots past the last value of rle_expand32 are unspecified (the caller allocates SIFT_CC_PAD).
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
range8_avx2(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	const uint8 *v = a;
	uint32		nw = n / 64;
	__m256i		vlo = _mm256_set1_epi8((char) lo),
				vsp = _mm256_set1_epi8((char) span);

	for (uint32 w = 0; w < nw; w++)
	{
		__m256i		x0 = _mm256_sub_epi8(LD256(v + w * 64), vlo),
					x1 = _mm256_sub_epi8(LD256(v + w * 64 + 32), vlo);
		uint32		m0 = (uint32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_min_epu8(x0, vsp), x0)),
					m1 = (uint32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_min_epu8(x1, vsp), x1));

		bits[w] = (uint64) m0 | (uint64) m1 << 32;
	}
	if (n % 64)
		sift_cc_kernels_scalar.range[0](v + (size_t) nw * 64, n % 64, lo, span, bits + nw);
}

CC_INLINE uint32
mask16x32(const uint16 *v, __m256i vlo, __m256i vsp)
{
	__m256i		x0 = _mm256_sub_epi16(LD256(v), vlo),
				x1 = _mm256_sub_epi16(LD256(v + 16), vlo),
				c0 = _mm256_cmpeq_epi16(_mm256_min_epu16(x0, vsp), x0),
				c1 = _mm256_cmpeq_epi16(_mm256_min_epu16(x1, vsp), x1),
				p = _mm256_permute4x64_epi64(_mm256_packs_epi16(c0, c1), 0xD8);

	return (uint32) _mm256_movemask_epi8(p);
}

static void
range16_avx2(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	const uint16 *v = a;
	uint32		nw = n / 64;
	__m256i		vlo = _mm256_set1_epi16((short) lo),
				vsp = _mm256_set1_epi16((short) span);

	for (uint32 w = 0; w < nw; w++)
		bits[w] = (uint64) mask16x32(v + w * 64, vlo, vsp) | (uint64) mask16x32(v + w * 64 + 32, vlo, vsp) << 32;
	if (n % 64)
		sift_cc_kernels_scalar.range[1](v + (size_t) nw * 64, n % 64, lo, span, bits + nw);
}

static void
range32_avx2(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	const uint32 *v = a;
	uint32		nw = n / 64;
	__m256i		vlo = _mm256_set1_epi32((int) lo),
				vsp = _mm256_set1_epi32((int) span);

	for (uint32 w = 0; w < nw; w++)
	{
		uint64		m = 0;

		for (int q = 0; q < 8; q++)
		{
			__m256i		x = _mm256_sub_epi32(LD256(v + w * 64 + q * 8), vlo);

			m |= (uint64) _mm256_movemask_ps(_mm256_castsi256_ps(le_epu32(x, vsp))) << (8 * q);
		}
		bits[w] = m;
	}
	if (n % 64)
		sift_cc_kernels_scalar.range[2](v + (size_t) nw * 64, n % 64, lo, span, bits + nw);
}

static void
range64_avx2(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	const uint64 *v = a;
	uint32		nw = n / 64;
	__m256i		vlo = _mm256_set1_epi64x((long long) lo),
				sign = _mm256_set1_epi64x((long long) UINT64CONST(0x8000000000000000)),
				vsp = _mm256_xor_si256(_mm256_set1_epi64x((long long) span), sign);

	for (uint32 w = 0; w < nw; w++)
	{
		uint64		m = 0;

		for (int q = 0; q < 16; q++)
		{
			__m256i		x = _mm256_xor_si256(_mm256_sub_epi64(LD256(v + w * 64 + q * 4), vlo), sign),
						gt = _mm256_cmpgt_epi64(x, vsp);

			m |= (uint64) (~_mm256_movemask_pd(_mm256_castsi256_pd(gt)) & 0xF) << (4 * q);
		}
		bits[w] = m;
	}
	if (n % 64)
		sift_cc_kernels_scalar.range[3](v + (size_t) nw * 64, n % 64, lo, span, bits + nw);
}

static void
widen_add64_avx2(const uint32 *a, uint32 n, int64 base, int64 *dst)
{
	__m256i		vb = _mm256_set1_epi64x((long long) base);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
		ST256(dst + i, _mm256_add_epi64(_mm256_cvtepu32_epi64(_mm_loadu_si128((const __m128i *) (a + i))), vb));
	sift_cc_kernels_scalar.widen_add64(a + i, n - i, base, dst + i);
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

static void
gather32_avx2(const uint32 *dict, const uint32 *codes, uint32 n, uint32 *dst)
{
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
		ST256(dst + i, _mm256_i32gather_epi32((const int *) dict, LD256(codes + i), 4));
	sift_cc_kernels_scalar.gather32(dict, codes + i, n - i, dst + i);
}

static void
gather64_avx2(const int64 *dict, const uint32 *codes, uint32 n, int64 *dst)
{
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
		ST256(dst + i, _mm256_i32gather_epi64((const long long *) dict, _mm_loadu_si128((const __m128i *) (codes + i)), 8));
	sift_cc_kernels_scalar.gather64(dict, codes + i, n - i, dst + i);
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
	.range = {range8_avx2, range16_avx2, range32_avx2, range64_avx2},
	.widen_add64 = widen_add64_avx2,
	.prefix32 = prefix32_avx2,
	.prefix64 = prefix64_avx2,
	.gather32 = gather32_avx2,
	.gather64 = gather64_avx2,
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
