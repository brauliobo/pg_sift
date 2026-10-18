/*
 * sift_shred_kern_avx2.c - AVX2 kernels of the shredder output; the results equal sift_shred_kern.c's.
 */
#include "sift_host.h"

#include "sift_shred.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K2 static SIFT_TARGET_AVX2

K2 void
state_counts_avx2(const uint8 *state, uint32 n, uint32 counts[4])
{
	uint32		c0 = 0,
				c1 = 0,
				c2 = 0,
				c3 = 0,
				i = 0;
	const __m256i s1 = _mm256_set1_epi8(1),
				s2 = _mm256_set1_epi8(2),
				s3 = _mm256_set1_epi8(3);

	for (; i + 32 <= n; i += 32)
	{
		__m256i		v = _mm256_loadu_si256((const __m256i *) (state + i));

		c0 += _mm_popcnt_u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_setzero_si256())));
		c1 += _mm_popcnt_u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s1)));
		c2 += _mm_popcnt_u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s2)));
		c3 += _mm_popcnt_u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s3)));
	}
	for (; i < n; i++)
	{
		c0 += state[i] == 0;
		c1 += state[i] == 1;
		c2 += state[i] == 2;
		c3 += state[i] == 3;
	}
	counts[0] = c0;
	counts[1] = c1;
	counts[2] = c2;
	counts[3] = c3;
}

/* The four state bytes at p as four 64-bit lanes that are all ones where the state is VALUE */
K2 __m256i
value_lanes(const uint8 *p)
{
	int			w;
	__m128i		b;

	memcpy(&w, p, 4);
	b = _mm_cvtsi32_si128(w);
	return _mm256_cmpeq_epi64(_mm256_cvtepu8_epi64(b), _mm256_set1_epi64x(SIFT_SHRED_VALUE));
}

K2 bool
minmax_i64_avx2(const uint8 *state, const int64 *val, uint32 n, int64 *min, int64 *max)
{
	__m256i		mn = _mm256_set1_epi64x(PG_INT64_MAX),
				mx = _mm256_set1_epi64x(PG_INT64_MIN),
				any = _mm256_setzero_si256();
	uint32		i = 0;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	bool		found;
	int64		t[4];

	for (; i + 4 <= n; i += 4)
	{
		__m256i		m = value_lanes(state + i),
					v = _mm256_loadu_si256((const __m256i *) (val + i)),
					a = _mm256_blendv_epi8(_mm256_set1_epi64x(PG_INT64_MAX), v, m),
					b = _mm256_blendv_epi8(_mm256_set1_epi64x(PG_INT64_MIN), v, m);

		mn = _mm256_blendv_epi8(mn, a, _mm256_cmpgt_epi64(mn, a));
		mx = _mm256_blendv_epi8(mx, b, _mm256_cmpgt_epi64(b, mx));
		any = _mm256_or_si256(any, m);
	}
	found = !_mm256_testz_si256(any, any);
	_mm256_storeu_si256((__m256i *) t, mn);
	for (int k = 0; k < 4; k++)
		lo = Min(lo, t[k]);
	_mm256_storeu_si256((__m256i *) t, mx);
	for (int k = 0; k < 4; k++)
		hi = Max(hi, t[k]);
	for (; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		lo = Min(lo, val[i]);
		hi = Max(hi, val[i]);
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

K2 bool
minmax_f64_avx2(const uint8 *state, const double *val, uint32 n, double *min, double *max)
{
	const __m256d pinf = _mm256_set1_pd(__builtin_inf()),
				ninf = _mm256_set1_pd(-__builtin_inf());
	__m256d		mn = pinf,
				mx = ninf;
	__m256i		any = _mm256_setzero_si256();
	uint32		i = 0;
	double		lo = __builtin_inf(),
				hi = -__builtin_inf(),
				t[4];
	bool		found;

	for (; i + 4 <= n; i += 4)
	{
		__m256i		m = value_lanes(state + i);
		__m256d		v = _mm256_loadu_pd(val + i),
					md = _mm256_castsi256_pd(m);

		mn = _mm256_min_pd(mn, _mm256_blendv_pd(pinf, v, md));
		mx = _mm256_max_pd(mx, _mm256_blendv_pd(ninf, v, md));
		any = _mm256_or_si256(any, m);
	}
	found = !_mm256_testz_si256(any, any);
	_mm256_storeu_pd(t, mn);
	for (int k = 0; k < 4; k++)
		lo = t[k] < lo ? t[k] : lo;
	_mm256_storeu_pd(t, mx);
	for (int k = 0; k < 4; k++)
		hi = t[k] > hi ? t[k] : hi;
	for (; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		lo = val[i] < lo ? val[i] : lo;
		hi = val[i] > hi ? val[i] : hi;
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

K2 void
expand_parents_avx2(const uint32 *end, uint32 np, uint32 *parent)
{
	uint32		i = 0;

	for (uint32 p = 0; p < np; p++)
	{
		uint32		e = end[p];

		if (e - i >= 8)
		{
			__m256i		v = _mm256_set1_epi32((int) p);

			for (; i + 8 <= e; i += 8)
				_mm256_storeu_si256((__m256i *) (parent + i), v);
		}
		for (; i < e; i++)
			parent[i] = p;
	}
}

const SiftShredKernels sift_shred_kernels_avx2 = {
	.state_counts = state_counts_avx2,
	.minmax_i64 = minmax_i64_avx2,
	.minmax_f64 = minmax_f64_avx2,
	.expand_parents = expand_parents_avx2
};

#endif
