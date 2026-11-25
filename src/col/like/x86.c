/*
 * col/like/x86.c - the substring search and the high byte test on 32 bytes at a time (AVX2).
 */
#include "core/host.h"

#include "core/cpu.h"

#include "col/like/simd.h"

#if defined(__x86_64__)

#include <immintrin.h>

SIFT_TARGET_AVX2 int64
sift_like_find_avx2(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen)
{
	const size_t m = nd->len;
	const __m256i vf = _mm256_set1_epi8((char) nd->first),
				vl = _mm256_set1_epi8((char) nd->last),
				of = _mm256_set1_epi8((char) nd->first_or),
				ol = _mm256_set1_epi8((char) nd->last_or);
	size_t		i = 0,
				spent = 0;

	while (i + m - 1 + 32 <= hlen)
	{
		__m256i		a = _mm256_loadu_si256((const __m256i *) (hay + i)),
					b = _mm256_loadu_si256((const __m256i *) (hay + i + m - 1));
		uint32		mask = (uint32) _mm256_movemask_epi8(_mm256_and_si256(_mm256_cmpeq_epi8(_mm256_or_si256(a, of), vf), _mm256_cmpeq_epi8(_mm256_or_si256(b, ol), vl)));

		while (mask != 0)
		{
			uint32		p = (uint32) __builtin_ctz(mask);

			if (sift_like_verify(nd, hay + i + p))
				return (int64) (i + p);
			spent += m;
			if (spent > SIFT_LIKE_VERIFY_BUDGET(i))
			{
				const int64 r = sift_like_find_twoway(nd, hay + i, hlen - i);

				return r < 0 ? -1 : (int64) i + r;
			}
			mask &= mask - 1;
		}
		i += 32;
	}
	{
		int64		r = sift_like_find_scalar(nd, hay + i, hlen - i);

		return r < 0 ? -1 : (int64) i + r;
	}
}

SIFT_TARGET_AVX2 bool
sift_like_has_high_avx2(const uint8 *p, size_t len)
{
	size_t		i = 0;

	for (; i + 32 <= len; i += 32)
		if (_mm256_movemask_epi8(_mm256_loadu_si256((const __m256i *) (p + i))) != 0)
			return true;
	for (; i < len; i++)
		if (p[i] & 0x80)
			return true;
	return false;
}

#endif
