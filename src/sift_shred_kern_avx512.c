/*
 * sift_shred_kern_avx512.c - AVX-512 kernels of the shredder output (masked loads and stores: no scalar tails); the results
 * equal sift_shred_kern.c's.
 */
#include "sift_host.h"

#include "sift_shred.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K5 static SIFT_TARGET_AVX512

/* The low n (<= 64) bits set */
#define LOWBITS(n)	((n) >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (n)) - 1)

K5 void
state_counts_avx512(const uint8 *state, uint32 n, uint32 counts[4])
{
	uint32		c[4] = {0, 0, 0, 0};

	for (uint32 i = 0; i < n; i += 64)
	{
		__mmask64	valid = LOWBITS(n - i);
		__m512i		v = _mm512_maskz_loadu_epi8(valid, state + i);

		c[0] += _mm_popcnt_u64(_mm512_mask_cmpeq_epi8_mask(valid, v, _mm512_setzero_si512()));
		c[1] += _mm_popcnt_u64(_mm512_mask_cmpeq_epi8_mask(valid, v, _mm512_set1_epi8(1)));
		c[2] += _mm_popcnt_u64(_mm512_mask_cmpeq_epi8_mask(valid, v, _mm512_set1_epi8(2)));
		c[3] += _mm_popcnt_u64(_mm512_mask_cmpeq_epi8_mask(valid, v, _mm512_set1_epi8(3)));
	}
	memcpy(counts, c, sizeof(c));
}

K5 bool
minmax_i64_avx512(const uint8 *state, const int64 *val, uint32 n, int64 *min, int64 *max)
{
	__m512i		mn = _mm512_set1_epi64(PG_INT64_MAX),
				mx = _mm512_set1_epi64(PG_INT64_MIN);
	__mmask8	any = 0;

	for (uint32 i = 0; i < n; i += 8)
	{
		__mmask8	valid = (__mmask8) LOWBITS(n - i);
		__m128i		s = _mm_maskz_loadu_epi8(valid, state + i);
		__mmask8	k = _mm_mask_cmpeq_epi8_mask(valid, s, _mm_set1_epi8(SIFT_SHRED_VALUE));
		__m512i		v = _mm512_maskz_loadu_epi64(k, val + i);

		mn = _mm512_mask_min_epi64(mn, k, mn, v);
		mx = _mm512_mask_max_epi64(mx, k, mx, v);
		any |= k;
	}
	if (any)
	{
		*min = _mm512_reduce_min_epi64(mn);
		*max = _mm512_reduce_max_epi64(mx);
	}
	return any != 0;
}

K5 bool
minmax_f64_avx512(const uint8 *state, const double *val, uint32 n, double *min, double *max)
{
	__m512d		mn = _mm512_set1_pd(__builtin_inf()),
				mx = _mm512_set1_pd(-__builtin_inf());
	__mmask8	any = 0;

	for (uint32 i = 0; i < n; i += 8)
	{
		__mmask8	valid = (__mmask8) LOWBITS(n - i);
		__m128i		s = _mm_maskz_loadu_epi8(valid, state + i);
		__mmask8	k = _mm_mask_cmpeq_epi8_mask(valid, s, _mm_set1_epi8(SIFT_SHRED_VALUE));
		__m512d		v = _mm512_maskz_loadu_pd(k, val + i);

		mn = _mm512_mask_min_pd(mn, k, mn, v);
		mx = _mm512_mask_max_pd(mx, k, mx, v);
		any |= k;
	}
	if (any)
	{
		*min = _mm512_reduce_min_pd(mn);
		*max = _mm512_reduce_max_pd(mx);
	}
	return any != 0;
}

K5 void
expand_parents_avx512(const uint32 *end, uint32 np, uint32 *parent)
{
	uint32		i = 0;

	for (uint32 p = 0; p < np; p++)
	{
		__m512i		v = _mm512_set1_epi32((int) p);

		for (; i < end[p]; i += 16)
			_mm512_mask_storeu_epi32(parent + i, (__mmask16) LOWBITS(end[p] - i), v);
		i = end[p];
	}
}

const SiftShredKernels sift_shred_kernels_avx512 = {
	.state_counts = state_counts_avx512,
	.minmax_i64 = minmax_i64_avx512,
	.minmax_f64 = minmax_f64_avx512,
	.expand_parents = expand_parents_avx512
};

#endif
