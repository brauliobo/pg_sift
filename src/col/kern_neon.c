/*
 * col/kern_neon.c - AArch64 Advanced SIMD column kernels; every slot is bit-identical to the scalar table,
 * which col/kern.c lists for the slots without a NEON kernel.
 *
 * NEON has no movemask: the compare results (all ones or zero per element) are narrowed to bytes, ANDed with the weight of their
 * bit in the byte and folded by pairwise adds into the 64-bit word. A range test is one wrapping subtraction and one unsigned
 * compare. The 8-bit LUT is a two-register table lookup (TBL). The last partial word of a bit vector is computed from a zero
 * padded copy of the remaining elements.
 */
#include "core/host.h"

#define SIFT_VEC_NEON
#include "col/kern_int.h"
#include "core/neon.h"
#include "core/simd_isa.h"

#if defined(SIFT_HAVE_NEON)

#define RANGE_NEON(name, T, vec, dup, W, ld, CMP) \
void \
name(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits) \
{ \
	const vec	l = dup((T) lo), \
				s = dup((T) span); \
	const T    *p_ = a; \
	uint32		i_ = 0; \
	for (; i_ + 64 <= n; i_ += 64) \
		bits[i_ / 64] = SIFT_NEON_WORD(W, ld, p_ + i_, CMP); \
	if (i_ < n) \
	{ \
		T			tmp_[64]; \
		uint32		r_ = n - i_; \
		memcpy(tmp_, p_ + i_, r_ * sizeof(T)); \
		memset(tmp_ + r_, 0, (64 - r_) * sizeof(T)); \
		bits[i_ / 64] = SIFT_NEON_WORD(W, ld, tmp_, CMP) & sift_colk_tail(n, i_ / 64); \
	} \
	sift_colk_zero_pad(bits, SIFT_COLK_WORDS(n), n); \
}

/* (x - l) <= s unsigned, with l and s in scope of the kernel */
#define CMP_U8(x)	vcleq_u8(vsubq_u8(x, l), s)
#define CMP_U16(x)	vcleq_u16(vsubq_u16(x, l), s)
#define CMP_U32(x)	vcleq_u32(vsubq_u32(x, l), s)
#define CMP_U64(x)	vcleq_u64(vsubq_u64(x, l), s)

RANGE_NEON(sift_colk_range_u8_neon, uint8, uint8x16_t, vdupq_n_u8, 8, vld1q_u8, CMP_U8)
RANGE_NEON(sift_colk_range_u16_neon, uint16, uint16x8_t, vdupq_n_u16, 16, vld1q_u16, CMP_U16)
RANGE_NEON(sift_colk_range_u32_neon, uint32, uint32x4_t, vdupq_n_u32, 32, vld1q_u32, CMP_U32)
RANGE_NEON(sift_colk_range_u64_neon, uint64, uint64x2_t, vdupq_n_u64, 64, vld1q_u64, CMP_U64)

/* The 8-bit LUT: the byte of the set that holds bit x (TBL over the 32 bytes of the set) tested against the bit's weight */
static inline uint8x16_t
lut8_mask(const uint8 *p, uint8x16x2_t set, uint8x16_t w)
{
	uint8x16_t	x = vld1q_u8(p);

	return vtstq_u8(vqtbl2q_u8(set, vshrq_n_u8(x, 3)), vqtbl1q_u8(w, vandq_u8(x, vdupq_n_u8(7))));
}

static inline uint64
lut8_word(const uint8 *p, uint8x16x2_t set, uint8x16_t w)
{
	return sift_neon_movemask64(lut8_mask(p, set, w), lut8_mask(p + 16, set, w), lut8_mask(p + 32, set, w), lut8_mask(p + 48, set, w));
}

void
sift_colk_lut_u8_neon(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	const uint8x16x2_t set = {{vld1q_u8((const uint8 *) lutbits), vld1q_u8((const uint8 *) lutbits + 16)}};
	const uint8x16_t w = vld1q_u8(sift_neon_bit_weight);

	SIFT_COLK_BITS_OF(uint8, a, n, bits, lut8_word, set, w);
}

/* A word with few bits is walked bit by bit; a denser one is expanded a byte at a time through the 256 x 8 position table */
uint32
sift_colk_bits_to_idx_neon(const uint64 *bits, uint32 nwords, uint32 base, uint32 *idx)
{
	uint32	   *o = idx;

	for (uint32 w = 0; w < nwords; w++, base += 64)
	{
		uint64		m = bits[w];

		if (__builtin_popcountll(m) <= SIFT_COLK_SEL_SPARSE_NEON)
		{
			for (; m; m &= m - 1)
				*o++ = base + __builtin_ctzll(m);
			continue;
		}
		SIFT_COLK_UNROLL(8)
		for (uint32 j = 0; j < 8; j++)
		{
			const uint32 b = (m >> (8 * j)) & 0xFF;
			const uint16x8_t pos = vmovl_u8(vld1_u8(sift_colk_lut_pos[b]));
			const uint32x4_t at = vdupq_n_u32(base + 8 * j);

			vst1q_u32(o, vaddq_u32(vmovl_u16(vget_low_u16(pos)), at));
			vst1q_u32(o + 4, vaddq_u32(vmovl_high_u16(pos), at));
			o += __builtin_popcount(b);
		}
	}
	return o - idx;
}

/* Bit i = !(mx[i] < lo || mn[i] > hi), signed in the key domain: the masks of the units that drop out, inverted once packed */
static inline uint8x16_t
zone16(const int64 *mn, const int64 *mx, int64x2_t lo, int64x2_t hi)
{
#define Z2(k) vorrq_u64(vcltq_s64(vld1q_s64(mx + (k)), lo), vcgtq_s64(vld1q_s64(mn + (k)), hi))
	return vmvnq_u8(sift_neon_pack16(sift_neon_pack32(sift_neon_pack64(Z2(0), Z2(2)), sift_neon_pack64(Z2(4), Z2(6))), sift_neon_pack32(sift_neon_pack64(Z2(8), Z2(10)), sift_neon_pack64(Z2(12), Z2(14)))));
#undef Z2
}

static inline uint64
zone_range_w(const int64 *mn, const int64 *mx, int64x2_t lo, int64x2_t hi)
{
	return sift_neon_movemask64(zone16(mn, mx, lo, hi), zone16(mn + 16, mx + 16, lo, hi), zone16(mn + 32, mx + 32, lo, hi), zone16(mn + 48, mx + 48, lo, hi));
}

void
sift_colk_zone_range_neon(const int64 *mn, const int64 *mx, uint32 n, int64 lo, int64 hi, uint64 *keep)
{
	const int64x2_t l = vdupq_n_s64(lo),
				h = vdupq_n_s64(hi);
	uint32		i = 0;

	for (; i + 64 <= n; i += 64)
		keep[i / 64] = zone_range_w(mn + i, mx + i, l, h);
	if (i < n)
	{
		int64		tn[64],
					tx[64];
		uint32		r = n - i;

		memcpy(tn, mn + i, r * sizeof(int64));
		memcpy(tx, mx + i, r * sizeof(int64));
		memset(tn + r, 0, (64 - r) * sizeof(int64));
		memset(tx + r, 0, (64 - r) * sizeof(int64));
		keep[i / 64] = zone_range_w(tn, tx, l, h) & sift_colk_tail(n, i / 64);
	}
	sift_colk_zero_pad(keep, SIFT_COLK_WORDS(n), n);
}

static inline uint8x16_t
notnull16(const uint32 *nn, const uint32 *nv)
{
#define N4(k) vmvnq_u32(vceqq_u32(vld1q_u32(nn + (k)), vld1q_u32(nv + (k))))
	return sift_neon_pack16(sift_neon_pack32(N4(0), N4(4)), sift_neon_pack32(N4(8), N4(12)));
#undef N4
}

static inline uint64
notnull_w(const uint32 *nn, const uint32 *nv)
{
	return sift_neon_movemask64(notnull16(nn, nv), notnull16(nn + 16, nv + 16), notnull16(nn + 32, nv + 32), notnull16(nn + 48, nv + 48));
}

void
sift_colk_zone_notnull_neon(const uint32 *nnull, const uint32 *nvals, uint32 n, uint64 *keep)
{
	uint32		i = 0;

	for (; i + 64 <= n; i += 64)
		keep[i / 64] &= notnull_w(nnull + i, nvals + i);
	if (i < n)
	{
		uint32		tn[64],
					tv[64],
					r = n - i;

		memcpy(tn, nnull + i, r * sizeof(uint32));
		memcpy(tv, nvals + i, r * sizeof(uint32));
		memset(tn + r, 0, (64 - r) * sizeof(uint32));
		memset(tv + r, 0, (64 - r) * sizeof(uint32));
		keep[i / 64] &= notnull_w(tn, tv) | ~sift_colk_tail(n, i / 64);
	}
}

/*
 * Eight values a step, from widths up to 25 bits, whose value plus its 7 bit offset fits the 4 bytes TBL picks: the group of 8 values is
 * `bits` bytes, the first four are cut from the 16 bytes at its start and the last four from the 16 bytes at their byte, each value
 * shifted right by its offset in the byte (a negative shift count) and masked. Wider values and the last values are the scalar kernel's.
 */
void
sift_colk_unpack_neon(const uint8 *src, uint32 bits, uint32 n, uint32 *dst)
{
	uint32		i = 0;

	if (bits <= 25)
	{
		const uint32 off1 = (4 * bits) >> 3;
		uint8		pick[2][16];
		int32		shift[2][4];
		const uint32x4_t mask = vdupq_n_u32((1U << bits) - 1);

		for (uint32 k = 0; k < 8; k++)
		{
			const uint32 pos = k * bits - (k >= 4 ? 8 * off1 : 0);

			for (uint32 j = 0; j < 4; j++)
				pick[k / 4][(k % 4) * 4 + j] = (pos >> 3) + j;
			shift[k / 4][k % 4] = -(int32) (pos & 7);
		}
		{
			const uint8x16_t p0 = vld1q_u8(pick[0]),
						p1 = vld1q_u8(pick[1]);
			const int32x4_t s0 = vld1q_s32(shift[0]),
						s1 = vld1q_s32(shift[1]);

			for (; i + 8 <= n; i += 8)
			{
				const uint8 *g = src + (size_t) (i / 8) * bits;

				vst1q_u32(dst + i, vandq_u32(vshlq_u32(vreinterpretq_u32_u8(vqtbl1q_u8(vld1q_u8(g), p0)), s0), mask));
				vst1q_u32(dst + i + 4, vandq_u32(vshlq_u32(vreinterpretq_u32_u8(vqtbl1q_u8(vld1q_u8(g + off1), p1)), s1), mask));
			}
		}
	}
	sift_colk_unpack_scalar(src + (size_t) (i / 8) * bits, bits, n - i, dst + i);
}

/* Flags of 16 bits at a time: byte j of the vector is the byte of bits that holds its bit, tested against the bit's weight */
void
sift_colk_bits_to_flags_neon(const uint64 *bits, uint32 n, uint8 *flags)
{
	static const uint8 pick[4][16] = {
		{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3},
		{4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7}
	};
	const uint8x16_t w = vld1q_u8(sift_neon_bit_weight),
				one = vdupq_n_u8(1);

	for (uint32 i = 0; i < n; i += 64)
	{
		const uint8x16_t word = vreinterpretq_u8_u64(vdupq_n_u64(bits[i / 64]));

		for (int j = 0; j < 4; j++)
			vst1q_u8(flags + i + 16 * j, vandq_u8(vtstq_u8(vqtbl1q_u8(word, vld1q_u8(pick[j])), w), one));
	}
}

/* popcount(a & b): CNT per byte, widened by pairwise adds into two 64-bit lanes */
uint64
sift_colk_bits_and_count_neon(const uint64 *a, const uint64 *b, uint32 nwords)
{
	uint64x2_t	acc = vdupq_n_u64(0);
	uint64		c;
	uint32		i = 0;

	for (; i + 2 <= nwords; i += 2)
		acc = vpadalq_u32(acc, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(vandq_u64(vld1q_u64(a + i), vld1q_u64(b + i)))))));
	c = vaddvq_u64(acc);
	for (; i < nwords; i++)
		c += sift_pc64(a[i] & b[i]);
	return c;
}

#define CV_LANES(T)	(16 / sizeof(T))
#define CV_FN(x)	void sift_colk_##x##_neon
#include "col/kern_vec.h"

#define TV_FN(x)	void sift_colk_##x##_neon
#include "col/text_vec.h"

/* keep &= the sets that share a bit with the probe (4 words each); the bits from n on are left as they are */
void
sift_colk_zone_set_neon(const uint64 (*set)[4], uint32 n, const uint64 *probe, uint64 *keep)
{
	const uint64x2_t p01 = vld1q_u64(probe),
				p23 = vld1q_u64(probe + 2);

	for (uint32 w = 0; w < SIFT_COLK_WORDS(n); w++)
	{
		uint64		word = 0;

		for (uint32 j = 0, i = w * 64; j < 64 && i < n; j++, i++)
		{
			const uint64x2_t x = vorrq_u64(vandq_u64(vld1q_u64(set[i]), p01), vandq_u64(vld1q_u64(set[i] + 2), p23));

			word |= (uint64) ((vgetq_lane_u64(x, 0) | vgetq_lane_u64(x, 1)) != 0) << j;
		}
		keep[w] &= word | ~sift_colk_tail(n, w);
	}
}

/* parent[i] = the p whose run [end[p - 1], end[p]) holds i: each run is filled four entries a store */
void
sift_colk_expand_parents_neon(const uint32 *end, uint32 np, uint32 *parent)
{
	uint32		i = 0;

	for (uint32 p = 0; p < np; p++)
	{
		const uint32x4_t v = vdupq_n_u32(p);

		for (; i + 4 <= end[p]; i += 4)
			vst1q_u32(parent + i, v);
		for (; i < end[p]; i++)
			parent[i] = p;
	}
}

#endif
