/*
 * core/neon.h - what the AArch64 Advanced SIMD kernel files share. NEON has no movemask: a compare result (all ones or zero per element)
 * is narrowed to bytes, ANDed with the weight of its bit in the byte and folded by pairwise adds into the bits of a word.
 */
#ifndef SIFT_NEON_H
#define SIFT_NEON_H

#include "core/host.h"

#include "core/cpu.h"

#if defined(SIFT_HAVE_NEON)
#include <arm_neon.h>

static const uint8 sift_neon_bit_weight[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};

/* The 64 byte masks c0..c3 (16 each) as a word: bit k = byte k */
static inline uint64
sift_neon_movemask64(uint8x16_t c0, uint8x16_t c1, uint8x16_t c2, uint8x16_t c3)
{
	const uint8x16_t w = vld1q_u8(sift_neon_bit_weight);
	uint8x16_t	t0 = vpaddq_u8(vandq_u8(c0, w), vandq_u8(c1, w)),
				t1 = vpaddq_u8(vandq_u8(c2, w), vandq_u8(c3, w));

	t0 = vpaddq_u8(t0, t1);
	t0 = vpaddq_u8(t0, t0);
	return vgetq_lane_u64(vreinterpretq_u64_u8(t0), 0);
}

/* Two masks of a width as one of the half width (the low halves of their lanes) */
static inline uint32x4_t
sift_neon_pack64(uint64x2_t a, uint64x2_t b)
{
	return vuzp1q_u32(vreinterpretq_u32_u64(a), vreinterpretq_u32_u64(b));
}

static inline uint16x8_t
sift_neon_pack32(uint32x4_t a, uint32x4_t b)
{
	return vuzp1q_u16(vreinterpretq_u16_u32(a), vreinterpretq_u16_u32(b));
}

static inline uint8x16_t
sift_neon_pack16(uint16x8_t a, uint16x8_t b)
{
	return vuzp1q_u8(vreinterpretq_u8_u16(a), vreinterpretq_u8_u16(b));
}

/*
 * The byte masks of the 16 elements at p, by element width, and the word of the 64 elements at p: ld is the load intrinsic of the
 * element type, cmp(vector) the compare (a macro or function giving the unsigned all-ones/zero mask vector of the same width)
 */
#define SIFT_NEON_MASK16_8(ld, p, cmp) \
	cmp(ld(p))
#define SIFT_NEON_MASK16_16(ld, p, cmp) \
	sift_neon_pack16(cmp(ld(p)), cmp(ld((p) + 8)))
#define SIFT_NEON_MASK16_32(ld, p, cmp) \
	sift_neon_pack16(sift_neon_pack32(cmp(ld(p)), cmp(ld((p) + 4))), sift_neon_pack32(cmp(ld((p) + 8)), cmp(ld((p) + 12))))
#define SIFT_NEON_MASK16_64(ld, p, cmp) \
	sift_neon_pack16(sift_neon_pack32(sift_neon_pack64(cmp(ld(p)), cmp(ld((p) + 2))), sift_neon_pack64(cmp(ld((p) + 4)), cmp(ld((p) + 6)))), \
					 sift_neon_pack32(sift_neon_pack64(cmp(ld((p) + 8)), cmp(ld((p) + 10))), sift_neon_pack64(cmp(ld((p) + 12)), cmp(ld((p) + 14)))))

#define SIFT_NEON_WORD(W, ld, p, cmp) \
	sift_neon_movemask64(SIFT_NEON_MASK16_##W(ld, p, cmp), SIFT_NEON_MASK16_##W(ld, (p) + 16, cmp), \
						 SIFT_NEON_MASK16_##W(ld, (p) + 32, cmp), SIFT_NEON_MASK16_##W(ld, (p) + 48, cmp))

#endif							/* SIFT_HAVE_NEON */

#endif							/* SIFT_NEON_H */
