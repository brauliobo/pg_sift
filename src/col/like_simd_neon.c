/*
 * col/like_simd_neon.c - the substring search and the high byte test on 16 bytes at a time (NEON).
 */
#include "core/host.h"

#include "core/cpu.h"

#include "col/like_simd.h"

#if defined(SIFT_HAVE_NEON)

#include <arm_neon.h>

/* One nibble for each byte of a comparison result: the bytes that are 0xFF as the bits of a 64-bit word */
static inline uint64
nibbles(uint8x16_t eq)
{
	return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
}

int64
sift_like_find_neon(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen)
{
	const size_t m = nd->len;
	const uint8x16_t vf = vdupq_n_u8(nd->first),
				vl = vdupq_n_u8(nd->last),
				of = vdupq_n_u8(nd->first_or),
				ol = vdupq_n_u8(nd->last_or);
	size_t		i = 0;

	while (i + m - 1 + 16 <= hlen)
	{
		uint8x16_t	a = vld1q_u8(hay + i),
					b = vld1q_u8(hay + i + m - 1);
		uint64		mask = nibbles(vandq_u8(vceqq_u8(vorrq_u8(a, of), vf), vceqq_u8(vorrq_u8(b, ol), vl)));

		while (mask != 0)
		{
			uint32		tz = (uint32) __builtin_ctzll(mask);

			if (sift_like_verify(nd, hay + i + (tz >> 2)))
				return (int64) (i + (tz >> 2));
			mask &= ~(UINT64CONST(0xF) << (tz & ~3U));
		}
		i += 16;
	}
	{
		int64		r = sift_like_find_scalar(nd, hay + i, hlen - i);

		return r < 0 ? -1 : (int64) i + r;
	}
}

bool
sift_like_has_high_neon(const uint8 *p, size_t len)
{
	size_t		i = 0;

	for (; i + 16 <= len; i += 16)
		if (vmaxvq_u8(vld1q_u8(p + i)) & 0x80)
			return true;
	for (; i < len; i++)
		if (p[i] & 0x80)
			return true;
	return false;
}

#endif
