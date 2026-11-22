/*
 * agg/kern6_neon.c - AArch64 Advanced SIMD operators of the 2048-row engine: bitmap words two at a time with the byte popcount, the
 * selection vector from a byte table, the 64-bit hash on two lanes (the multiply is three widening products; a selection vector is a
 * pair of loads; four rows an iteration), and the sums split into 32-bit halves so that the lanes never carry (see agg/kern6_avx2.c).
 * The predicates are in agg/kern6_neon_cmp.c, the minimums, maximums and arithmetic in agg/kern6_neon_agg.c, the grouped accumulation
 * (NEON has no scatter) in agg/kern6_gid.h. Results are bit-identical to the scalar table (agg/kern6.c).
 */
#include "core/host.h"
#include "core/cpu.h"

#include "agg/kern6_neon.h"

#if defined(SIFT_HAVE_NEON)

/* The largest group of rows [from, n) of a batch without a selection vector, four at a time (the probe of the four banks of agg/kern6_gid.h) */
static inline uint32
gid_max_nosel(const uint32 *gid, int from, int n)
{
	uint32x4_t	m = vdupq_n_u32(0);
	uint32		r;
	int			i = from;

	for (; i + 4 <= n; i += 4)
		m = vmaxq_u32(m, vld1q_u32(gid + i));
	r = vmaxvq_u32(m);
	for (; i < n; i++)
		r = Max(r, gid[i]);
	return r;
}

#define AK6_GID_MAX_NOSEL	gid_max_nosel
#include "agg/kern6_gid.h"

static uint16
mask_andn_del_neon(uint64 *dst, const uint64 *del, uint16 n)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 2 <= full; w += 2)
	{
		uint64x2_t	x = vbicq_u64(vld1q_u64(dst + w), vld1q_u64(del + w));

		vst1q_u64(dst + w, x);
		pc += pc2(x);
	}
	for (; w < full; w++)
	{
		dst[w] &= ~del[w];
		pc += __builtin_popcountll(dst[w]);
	}
	if (w < nw)
	{
		dst[w] = dst[w] & ~del[w] & AK6_TAIL(n);
		pc += __builtin_popcountll(dst[w]);
	}
	return pc;
}

static uint16
mask_and_neon(uint64 *dst, const uint64 *a, const uint64 *b, uint16 n)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 2 <= full; w += 2)
	{
		uint64x2_t	x = vandq_u64(vld1q_u64(a + w), vld1q_u64(b + w));

		vst1q_u64(dst + w, x);
		pc += pc2(x);
	}
	for (; w < full; w++)
	{
		dst[w] = a[w] & b[w];
		pc += __builtin_popcountll(dst[w]);
	}
	if (w < nw)
	{
		dst[w] = a[w] & b[w] & AK6_TAIL(n);
		pc += __builtin_popcountll(dst[w]);
	}
	return pc;
}

static uint16
mask_to_sel16_neon(uint16 *sel, const uint64 *mask, uint16 n)
{
	const int	nw = AK6_NW(n);
	uint32		k = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		bits = mask[w] & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0));

		if (__builtin_popcountll(bits) <= 4)
		{
			for (; bits; bits &= bits - 1)
				sel[k++] = (uint16) (w * 64 + __builtin_ctzll(bits));
			continue;
		}
		for (int j = 0; j < 8; j++)
		{
			const uint32 b = (uint32) (bits >> (8 * j)) & 0xFF;
			uint16x8_t	idx = vaddq_u16(vmovl_u8(vld1_u8(ak6_byte_idx[b])), vdupq_n_u16((uint16) (w * 64 + 8 * j)));

			vst1q_u16(sel + k, idx);
			k += __builtin_popcount(b);
		}
	}
	return k;
}

static uint16
key_present_apply_neon(uint64 *dst, const uint64 *kp, const uint64 *doc_valid, uint16 n, bool negate)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 2 <= full; w += 2)
	{
		uint64x2_t	x = vandq_u64(vld1q_u64(dst + w), negate ? vbicq_u64(vld1q_u64(doc_valid + w), vld1q_u64(kp + w)) : vld1q_u64(kp + w));

		vst1q_u64(dst + w, x);
		pc += pc2(x);
	}
	for (; w < nw; w++)
	{
		dst[w] &= negate ? ~kp[w] & doc_valid[w] : kp[w];
		if (w == nw - 1)
			dst[w] &= AK6_TAIL(n);
		pc += __builtin_popcountll(dst[w]);
	}
	return pc;
}

/* ---- hashing: four rows an iteration (two independent chains), the last rows one at a time ---- */

static void
hash_i64_neon(uint64 *h, const int64 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		const uint64x2_t a = neon_hash_step(mix ? vld1q_u64(h + i) : vdupq_n_u64(0), ld64x2(v, sel, i), mix),
					b = neon_hash_step(mix ? vld1q_u64(h + i + 2) : vdupq_n_u64(0), ld64x2(v, sel, i + 2), mix);

		vst1q_u64(h + i, a);
		vst1q_u64(h + i + 2, b);
	}
	for (; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) v[ROW_OF_SEL(sel, i)], mix);
}

static void
hash_i32_neon(uint64 *h, const int32 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		const uint64x2_t a = neon_hash_step(mix ? vld1q_u64(h + i) : vdupq_n_u64(0), ld32x2(v, sel, i), mix),
					b = neon_hash_step(mix ? vld1q_u64(h + i + 2) : vdupq_n_u64(0), ld32x2(v, sel, i + 2), mix);

		vst1q_u64(h + i, a);
		vst1q_u64(h + i + 2, b);
	}
	for (; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) (int64) v[ROW_OF_SEL(sel, i)], mix);
}

/* Two 16 byte values of the active rows i, i + 1 as their halves: lo = (lo0, lo1), hi = (hi0, hi1) */
static inline void
ld128x2(const void *v, const uint16 *sel, int i, uint64x2_t *lo, uint64x2_t *hi)
{
	const uint64x2_t a = vld1q_u64((const uint64 *) ((const char *) v + 16 * (size_t) ROW_OF_SEL(sel, i))),
				b = vld1q_u64((const uint64 *) ((const char *) v + 16 * (size_t) ROW_OF_SEL(sel, i + 1)));

	*lo = vzip1q_u64(a, b);
	*hi = vzip2q_u64(a, b);
}

static void
hash_i128_neon(uint64 *h, const SiftI128 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 2 <= n; i += 2)
	{
		uint64x2_t	lo,
					hi;

		ld128x2(v, sel, i, &lo, &hi);
		vst1q_u64(h + i, neon_hash_step(neon_hash_step(mix ? vld1q_u64(h + i) : vdupq_n_u64(0), lo, mix), hi, true));
	}
	for (; i < n; i++)
	{
		const SiftI128 *x = &v[ROW_OF_SEL(sel, i)];

		h[i] = AK6_HASH_STEP(AK6_HASH_STEP(h[i], x->lo, mix), (uint64) x->hi, true);
	}
}

/* A view of at most 12 bytes hashes by its two halves with no dereference; two rows of which one is longer go through the scalar function */
static void
hash_strview_neon(uint64 *h, const SiftStrView *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 2 <= n; i += 2)
	{
		uint64x2_t	hdr,
					tail;

		ld128x2(v, sel, i, &hdr, &tail);
		if (v[ROW_OF_SEL(sel, i)].len > 12 || v[ROW_OF_SEL(sel, i + 1)].len > 12)
		{
			h[i] = ak6_sv_hash(h[i], &v[ROW_OF_SEL(sel, i)], mix);
			h[i + 1] = ak6_sv_hash(h[i + 1], &v[ROW_OF_SEL(sel, i + 1)], mix);
			continue;
		}
		vst1q_u64(h + i, neon_hash_step(neon_hash_step(mix ? vld1q_u64(h + i) : vdupq_n_u64(0), hdr, mix), tail, true));
	}
	for (; i < n; i++)
		h[i] = ak6_sv_hash(h[i], &v[ROW_OF_SEL(sel, i)], mix);
}

static int64
sum_i32_neon(const int32 *v, const uint64 *mask, uint16 n)
{
	int64x2_t	acc = vdupq_n_s64(0);
	int64		s;
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		int32x4_t	x = vld1q_s32(v + i);

		if (mask)
			x = vreinterpretq_s32_u32(vandq_u32(vreinterpretq_u32_s32(x), lanes4(BITS_AT(mask, i, 0xF))));
		acc = vaddw_s32(acc, vget_low_s32(x));
		acc = vaddw_high_s32(acc, x);
	}
	s = vaddvq_s64(acc);
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
			s += v[i];
	return s;
}

static SiftI128
sum_i64_to_i128_neon(const int64 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	const uint64x2_t lo32 = vdupq_n_u64(0xFFFFFFFF);
	uint64x2_t	a = vdupq_n_u64(0),
				b = a,
				c = a;
	ak6_u128	s = ak6_u128_of(init);
	int			i = 0;

	for (; i + 2 <= n; i += 2)
	{
		uint64x2_t	x = vreinterpretq_u64_s64(vld1q_s64(v + i)),
					u;

		if (mask)
			x = vandq_u64(x, lanes2(BITS_AT(mask, i, 3)));
		u = vshrq_n_u64(x, 32);
		a = vaddq_u64(a, vandq_u64(x, lo32));
		b = vaddq_u64(b, u);
		c = vaddq_u64(c, vshrq_n_u64(u, 31));
	}
	s += ak6_split_total(vaddvq_u64(a), vaddvq_u64(b), vaddvq_u64(c));
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
			s += (ak6_u128) (__int128) v[i];
	return ak6_i128_of(s);
}

static bool
sum_i64_checked_neon(const int64 *v, const uint64 *mask, uint16 n, int64 *out)
{
	const SiftI128 zero = {0, 0};

	return ak6_fits_i64(sum_i64_to_i128_neon(v, mask, n, zero), out);
}

static SiftI128
sum_i128_neon(const SiftI128 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	const uint64x2_t lo32 = vdupq_n_u64(0xFFFFFFFF);
	uint64x2_t	a = vdupq_n_u64(0),
				b = a;
	ak6_u128	s = ak6_u128_of(init);

	for (int i = 0; i < n; i++)
	{
		uint64x2_t	x = vld1q_u64((const uint64 *) (v + i));

		if (mask)
			x = vandq_u64(x, vdupq_n_u64(0 - (uint64) ROWBIT(mask, i)));
		a = vaddq_u64(a, vandq_u64(x, lo32));
		b = vaddq_u64(b, vshrq_n_u64(x, 32));
	}
	s += (ak6_u128) vgetq_lane_u64(a, 0) + ((ak6_u128) vgetq_lane_u64(b, 0) << 32);
	s += (ak6_u128) (vgetq_lane_u64(a, 1) + (vgetq_lane_u64(b, 1) << 32)) << 64;
	return ak6_i128_of(s);
}

static double
sum_f64_fast_neon(const double *v, const uint64 *mask, uint16 n, double init)
{
	float64x2_t acc[4] = {vdupq_n_f64(0), vdupq_n_f64(0), vdupq_n_f64(0), vdupq_n_f64(0)};
	double		a[8];
	int			i = 0;

	for (; i + 8 <= n; i += 8)
		for (int j = 0; j < 4; j++)
		{
			float64x2_t x = vld1q_f64(v + i + 2 * j);

			if (mask)
				x = vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(x), lanes2(BITS_AT(mask, i + 2 * j, 3))));
			acc[j] = vaddq_f64(acc[j], x);
		}
	for (int j = 0; j < 4; j++)
		vst1q_f64(a + 2 * j, acc[j]);
	for (; i < n; i++)
		a[i & 7] += (!mask || ROWBIT(mask, i)) ? v[i] : 0.0;
	return ak6_f64_tree(a, init);
}

#define AK6_INITN(s)	.s = s##_neon,
#define AK6_INITNX(s)	AK6_INIT_AT(s, neon)
#define AK6_INITNS(s)	.s = ak6_##s##_scalar,

#define AK6_NEON_COMMON \
	AK6_INITN(mask_andn_del) AK6_INITN(mask_and) AK6_INITN(mask_to_sel16) AK6_INITN(key_present_apply) \
	AK6_INITN(sum_i32) AK6_INITN(sum_i64_checked) AK6_INITN(sum_i64_to_i128) AK6_INITN(sum_f64_fast) \
	AK6_NEON_FILES(AK6_INITNX) \
	AK6_ADDED_SLOTS(AK6_INITNX) \
	.count_gid = ak6_g_count_gid, \
	.sum_gid_i32 = ak6_g_sum_gid_i32, \
	.sum_gid_i64 = ak6_g_sum_gid_i64, \
	.sum_gid_i64_to_i128 = ak6_g_sum_gid_i64_to_i128, \
	.sum_gid_i128 = ak6_g_sum_gid_i128, \
	.sum_gid_f64_fast = ak6_g_sum_gid_f64_fast, \
	.minmax_gid_i64 = ak6_g_minmax_gid_i64, \
	.minmax_gid_f64 = ak6_g_minmax_gid_f64,

/* the slots that are scalar in the table of the level: the hashes, the 128-bit sum, and AK6_NEON_VECTOR_FILES */
const SiftAggKernelsV6 sift_aggkern6_neon = {
	AK6_NEON_COMMON
	AK6_INITNS(hash_i32) AK6_INITNS(hash_i64) AK6_INITNS(hash_i128) AK6_INITNS(hash_strview) AK6_INITNS(sum_i128)
	AK6_NEON_VECTOR_FILES(AK6_INITNS)
};

const SiftAggKernelsV6 sift_aggkern6_neon_vector = {
	AK6_NEON_COMMON
	AK6_INITN(hash_i32) AK6_INITN(hash_i64) AK6_INITN(hash_i128) AK6_INITN(hash_strview) AK6_INITN(sum_i128)
	AK6_NEON_VECTOR_FILES(AK6_INITNX)
};

#endif							/* SIFT_HAVE_NEON */
