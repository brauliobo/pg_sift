/*
 * col/codec_avx2.c - AVX2 kernels of the chunk codec (x86-64-v3 function-level target, safe to link on any x86-64). Bit-identical to
 * the scalar table in col/codec_kern.c; slots past the last value of rle_expand32 are unspecified (the caller allocates SIFT_COL_PAD).
 */
#if defined(__x86_64__)
#include "core/host.h"

#include "col/codec_int.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#endif
#include <immintrin.h>

#define CC_VB 32
#define CC_FN(x) cc_##x##_avx2
#include "col/codec_vec.h"

#define LD256(p) _mm256_loadu_si256((const __m256i *) (p))
#define ST256(p, v) _mm256_storeu_si256((__m256i *) (p), (v))

/* unsigned x <= y per u32 lane */
CC_INLINE __m256i
le_epu32(__m256i x, __m256i y)
{
	return _mm256_cmpeq_epi32(_mm256_min_epu32(x, y), x);
}

/*
 * The rows of a block of 32 values (CC_NV vectors each) whose value is in [lo, lo + span], one bit each, 32 to a u32 of bits (little endian: the
 * byte order of x86-64): a row is shifted out of the lane streams and compared in registers, with no row array between
 */
static void
unpack_range32_avx2(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;
	uint32		row = 0;
	uint32	   *out = (uint32 *) bits;
	__m256i		vspan = _mm256_set1_epi32((int) span),
				vmask = _mm256_set1_epi32((int) mask),
				vadd = _mm256_set1_epi32((int) (0u - lo));

	if (rows & 1)
		out[rows] = 0;
	if (W == 0)
	{
		for (uint32 r = 0; r < rows; r++)
			out[r] = (0u - lo) <= span ? ~0u : 0;
		return;
	}
	while (row < rows)
	{
		uint32		R = rows - row > 32 ? 32 : rows - row;

		for (uint32 r = 0; r < R; r++, row++)
		{
			uint32		bp = r * W,
						sh = bp & 31,
						m;
			const uint32 *a = src + (size_t) (bp >> 5) * 32;
			__m128i		cnt = _mm_cvtsi32_si128((int) sh),
						cnt2 = _mm_cvtsi32_si128((int) (32 - sh));
			__m256i		x0 = _mm256_srl_epi32(LD256(a), cnt),
						x1 = _mm256_srl_epi32(LD256(a + 8), cnt),
						x2 = _mm256_srl_epi32(LD256(a + 16), cnt),
						x3 = _mm256_srl_epi32(LD256(a + 24), cnt);

			if (sh + W > 32)
			{
				x0 = _mm256_or_si256(x0, _mm256_sll_epi32(LD256(a + 32), cnt2));
				x1 = _mm256_or_si256(x1, _mm256_sll_epi32(LD256(a + 40), cnt2));
				x2 = _mm256_or_si256(x2, _mm256_sll_epi32(LD256(a + 48), cnt2));
				x3 = _mm256_or_si256(x3, _mm256_sll_epi32(LD256(a + 56), cnt2));
			}
			x0 = _mm256_add_epi32(_mm256_and_si256(x0, vmask), vadd);
			x1 = _mm256_add_epi32(_mm256_and_si256(x1, vmask), vadd);
			x2 = _mm256_add_epi32(_mm256_and_si256(x2, vmask), vadd);
			x3 = _mm256_add_epi32(_mm256_and_si256(x3, vmask), vadd);
			m = (uint32) _mm256_movemask_ps(_mm256_castsi256_ps(le_epu32(x0, vspan))) |
				(uint32) _mm256_movemask_ps(_mm256_castsi256_ps(le_epu32(x1, vspan))) << 8 |
				(uint32) _mm256_movemask_ps(_mm256_castsi256_ps(le_epu32(x2, vspan))) << 16 |
				(uint32) _mm256_movemask_ps(_mm256_castsi256_ps(le_epu32(x3, vspan))) << 24;
			out[row] = m;
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
alp_f8_avx2(double *dst, const uint32 *u, uint32 n, int64 dbase, double div)
{
	__m256d		off = _mm256_set1_pd(2147483648.0 + (double) dbase),
				vd = _mm256_set1_pd(div);
	__m128i		flip = _mm_set1_epi32((int) 0x80000000u);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m128i		x = _mm_xor_si128(_mm_loadu_si128((const __m128i *) (u + i)), flip);

		_mm256_storeu_pd(dst + i, _mm256_div_pd(_mm256_add_pd(_mm256_cvtepi32_pd(x), off), vd));
	}
	sift_cc_kernels_scalar.alp_f8(dst + i, u + i, n - i, dbase, div);
}

static void
alp_f4_avx2(double *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	__m256i		vb = _mm256_set1_epi32((int) (uint32) dbase);
	__m256		vd = _mm256_set1_ps(div);
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m256		x = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(LD256(u + i), vb)), vd);

		_mm256_storeu_pd(dst + i, _mm256_cvtps_pd(_mm256_castps256_ps128(x)));
		_mm256_storeu_pd(dst + i + 4, _mm256_cvtps_pd(_mm256_extractf128_ps(x, 1)));
	}
	sift_cc_kernels_scalar.alp_f4(dst + i, u + i, n - i, dbase, div);
}

static void
scale32_avx2(uint32 *a, uint32 n, uint32 base, uint32 mul)
{
	__m256i		vb = _mm256_set1_epi32((int) base),
				vm = _mm256_set1_epi32((int) mul);
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
		ST256(a + i, _mm256_add_epi32(_mm256_mullo_epi32(LD256(a + i), vm), vb));
	sift_cc_kernels_scalar.scale32(a + i, n - i, base, mul);
}

/* The low 64 bits of x * mul from 32 x 32 products: lo * lo + ((lo * hi + hi * lo) << 32) */
static void
scale64_avx2(int64 *a, uint32 n, int64 base, int64 mul)
{
	__m256i		vb = _mm256_set1_epi64x(base),
				vm = _mm256_set1_epi64x(mul),
				vmh = _mm256_srli_epi64(vm, 32);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m256i		x = LD256(a + i),
					cross = _mm256_add_epi64(_mm256_mul_epu32(x, vmh), _mm256_mul_epu32(_mm256_srli_epi64(x, 32), vm));

		ST256(a + i, _mm256_add_epi64(_mm256_add_epi64(_mm256_mul_epu32(x, vm), _mm256_slli_epi64(cross, 32)), vb));
	}
	sift_cc_kernels_scalar.scale64(a + i, n - i, base, mul);
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
	.scale32 = scale32_avx2,
	.scale64 = scale64_avx2,
	.alp_f8 = alp_f8_avx2,
	.alp_f4 = alp_f4_avx2,
};

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#endif							/* __x86_64__ */
