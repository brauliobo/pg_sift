/*
 * agg/kern6_neon.h - what the AArch64 Advanced SIMD files of the 2048-row operators share (agg/kern6_neon*.c): the 64-bit multiply
 * and the hash step on two lanes (NEON has no 64-bit vector multiply: it is three 32x32 -> 64 widening products), the movemask of a
 * compare, the lane masks of a bitmap word, and the float8 key of two doubles. The grouped accumulation is in agg/kern6_gid.h.
 */
#ifndef SIFT_AGGKERN6_NEON_H
#define SIFT_AGGKERN6_NEON_H

#include "agg/kern6_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

#define NK static inline __attribute__((always_inline))

#define BITS_AT(mask, i, m)	((uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & (m))
#define ROWBIT(mask, i)		AK6_ROWBIT(mask, i)

NK uint32
pc2(uint64x2_t x)
{
	return vaddvq_u8(vcntq_u8(vreinterpretq_u8_u64(x)));
}

NK uint64x2_t
lanes2(uint32 b)
{
	static const uint64 bit[2] = {1, 2};

	return vtstq_u64(vdupq_n_u64(b), vld1q_u64(bit));
}

NK uint32x4_t
lanes4(uint32 b)
{
	static const uint32 bit[4] = {1, 2, 4, 8};

	return vtstq_u32(vdupq_n_u32(b), vld1q_u32(bit));
}

/* The bits of a compare result: one per lane */
NK uint32
mm2(uint64x2_t m)
{
	static const uint64 w[2] = {1, 2};

	return (uint32) vaddvq_u64(vandq_u64(m, vld1q_u64(w)));
}

NK uint32
mm4(uint32x4_t m)
{
	static const uint32 w[4] = {1, 2, 4, 8};

	return vaddvq_u32(vandq_u32(m, vld1q_u32(w)));
}

NK uint64x2_t
not64(uint64x2_t m)
{
	return vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(m)));
}

/* The low 64 bits of a * b per lane: lo*lo + ((hi*lo + lo*hi) << 32) */
NK uint64x2_t
neon_mul64(uint64x2_t a, uint64x2_t b)
{
	const uint32x2_t alo = vmovn_u64(a),
				ahi = vshrn_n_u64(a, 32),
				blo = vmovn_u64(b),
				bhi = vshrn_n_u64(b, 32);
	const uint64x2_t cross = vmlal_u32(vmull_u32(ahi, blo), alo, bhi);

	return vaddq_u64(vmull_u32(alo, blo), vshlq_n_u64(cross, 32));
}

NK uint64x2_t
neon_hash1(uint64x2_t x)
{
	x = neon_mul64(veorq_u64(x, vshrq_n_u64(x, 32)), vdupq_n_u64(UINT64CONST(0x9E3779B97F4A7C15)));
	x = neon_mul64(veorq_u64(x, vshrq_n_u64(x, 29)), vdupq_n_u64(UINT64CONST(0xBF58476D1CE4E5B9)));
	return veorq_u64(x, vshrq_n_u64(x, 32));
}

NK uint64x2_t
neon_hash_step(uint64x2_t h, uint64x2_t x, bool mix)
{
	if (mix)
		x = veorq_u64(vorrq_u64(vshlq_n_u64(h, 5), vshrq_n_u64(h, 59)), x);
	return neon_hash1(x);
}

/* Two int64 of the active rows i, i + 1 */
NK uint64x2_t
ld64x2(const int64 *v, const uint16 *sel, int i)
{
	return sel ? vcombine_u64(vld1_u64((const uint64 *) &v[sel[i]]), vld1_u64((const uint64 *) &v[sel[i + 1]]))
		: vld1q_u64((const uint64 *) (v + i));
}

/* Two int32 of the active rows i, i + 1, sign-extended */
NK uint64x2_t
ld32x2(const int32 *v, const uint16 *sel, int i)
{
	return vreinterpretq_u64_s64(vmovl_s32(sel ? vset_lane_s32(v[sel[i + 1]], vdup_n_s32(v[sel[i]]), 1) : vld1_s32(v + i)));
}

/* The float8 key (ak6_f8_key) of two doubles: -0.0 as 0, every NaN the quiet NaN, then the low 63 bits flipped for a negative number */
NK int64x2_t
f8key2(float64x2_t x)
{
	const uint64x2_t nan = not64(vceqq_f64(x, x)),
				zero = vceqq_f64(x, vdupq_n_f64(0.0));
	const uint64x2_t b = vbslq_u64(nan, vdupq_n_u64(AK6_F8_NAN_BITS), vbicq_u64(vreinterpretq_u64_f64(x), zero));
	const uint64x2_t neg = vcltzq_s64(vreinterpretq_s64_u64(b));

	return vreinterpretq_s64_u64(veorq_u64(b, vandq_u64(neg, vdupq_n_u64(AK6_F8_FLIP))));
}

/*
 * The slots of the level that its other files define (declared for them and for the tables). The slots of AK6_NEON_VECTOR are the ones whose vector
 * kernel is slower than the scalar function on the core measured (Apple M4, whose scalar 64-bit multiply and 128-bit compare are single instructions: the
 * vector multiply is three widening products): the table of the level has the scalar function there and sift_aggkern6_neon_vector the vector kernel,
 * for a core where it wins.
 */
#define AK6_NEON_FILES(X) \
	X(cmp_i32) X(cmp_i64) X(cmp_f64) X(cmp2_i32) X(cmp2_i64) X(cmp2_f64) X(lut_u32) X(in_set_i64) X(stream_run_ne_i64) X(minmax_i32) \
	X(minmax_i64) X(minmax_f64)
#define AK6_NEON_VECTOR_FILES(X) \
	X(cmp_i128) X(cmp_strview_eq) X(minmax_i128) X(lin_i64) X(mulw_i64)
#define AK6_DECLN(s)	AK6_DECLARE_AT(s, neon)
AK6_NEON_FILES(AK6_DECLN)
AK6_NEON_VECTOR_FILES(AK6_DECLN)

#endif							/* SIFT_HAVE_NEON */

#endif							/* SIFT_AGGKERN6_NEON_H */
