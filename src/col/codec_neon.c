/*
 * col/codec_neon.c - AArch64 Advanced SIMD kernels of the chunk codec. Bit-identical to the scalar table in col/codec_kern.c; slots past the
 * last value of rle_expand32 are unspecified (the caller allocates SIFT_COL_PAD).
 *
 * Only the slots that win on the target CPU (arm64, ns for 4096 values, scalar -> NEON): unpack32 (W 3..20) 1500 -> 195..250, unpack_range32
 * 1750..2125 -> 312..375, rle_expand32 1125 -> 273, alp_keys_f4 1375 -> 719. The others stay the scalar functions because the compiler already
 * vectorizes them or the NEON loop is slower: max32 and prefix32 tie (105 and 1313 ns), scale32 133 -> 145, scale64 422 -> 906, prefix64 1813 -> 2625,
 * alp_keys_f8 1000 -> 1563, alp_f8 453 -> 1250, alp_f4 453 -> 547.
 */
#include "core/host.h"

#include "col/codec_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

#define CC_VB 16
#define CC_FN(x) cc_##x##_neon
#include "col/codec_vec.h"

#define LD4(p)		vld1q_u32(p)
#define ST4(p, v)	vst1q_u32((p), (v))

/* the 32 bits of eight u32 masks (all ones or zero per lane), lane k to bit k */
CC_INLINE uint32
mask32(uint32x4_t c0, uint32x4_t c1, uint32x4_t c2, uint32x4_t c3, uint32x4_t c4, uint32x4_t c5, uint32x4_t c6, uint32x4_t c7)
{
	uint8x16_t	z = vdupq_n_u8(0);

	return (uint32) sift_neon_movemask64(sift_neon_pack16(sift_neon_pack32(c0, c1), sift_neon_pack32(c2, c3)),
										 sift_neon_pack16(sift_neon_pack32(c4, c5), sift_neon_pack32(c6, c7)), z, z);
}

/* The rows of a block of 32 values whose value is in [lo, lo + span], one bit each, 32 to a u32 of bits (little endian): as the AVX2 kernel, a row is shifted out of the lane streams and compared in registers */
static void
cc_unpack_range32_neon(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;
	uint32		row = 0;
	uint32	   *out = (uint32 *) bits;
	uint32x4_t	vspan = vdupq_n_u32(span),
				vmask = vdupq_n_u32(mask),
				vadd = vdupq_n_u32(0u - lo);

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
						sh = bp & 31;
			const uint32 *a = src + (size_t) (bp >> 5) * 32;
			int32x4_t	right = vdupq_n_s32(-(int) sh),
						left = vdupq_n_s32((int) (32 - sh));
			uint32x4_t	x[8];

			for (int q = 0; q < 8; q++)
			{
				x[q] = vshlq_u32(LD4(a + q * 4), right);
				if (sh + W > 32)
					x[q] = vorrq_u32(x[q], vshlq_u32(LD4(a + 32 + q * 4), left));
				x[q] = vcleq_u32(vaddq_u32(vandq_u32(x[q], vmask), vadd), vspan);
			}
			out[row] = mask32(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
	}
}

/* key of two doubles: bits ^ ((bits >> 63) >> 1) with an arithmetic shift of the sign */
CC_INLINE uint64x2_t
keys2(float64x2_t x)
{
	int64x2_t	b = vreinterpretq_s64_f64(x);

	return veorq_u64(vreinterpretq_u64_s64(b), vshrq_n_u64(vreinterpretq_u64_s64(vshrq_n_s64(b, 63)), 1));
}

static void
cc_alp_keys_f4_neon(int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	uint32x4_t	vb = vdupq_n_u32((uint32) dbase);
	float32x4_t	vd = vdupq_n_f32(div);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
	{
		float32x4_t x = vdivq_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(vaddq_u32(LD4(u + i), vb))), vd);

		vst1q_u64((uint64 *) (dst + i), keys2(vcvt_f64_f32(vget_low_f32(x))));
		vst1q_u64((uint64 *) (dst + i + 2), keys2(vcvt_high_f64_f32(x)));
	}
	sift_cc_kernels_scalar.alp_keys_f4(dst + i, u + i, n - i, dbase, div);
}

static void
cc_rle_expand32_neon(const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst)
{
	for (uint32 r = 0; r < nruns; r++)
	{
		uint32x4_t	v = vdupq_n_u32(vals[r]);
		uint32		len = lens[r];

		for (uint32 k = 0; k < len; k += 4)
			ST4(dst + k, v);
		dst += len;
	}
}

/* ... exported for the NEON table of col/codec_kern.c, which keeps the scalar kernels for the other slots */
void
sift_cc_unpack32_neon(const uint32 *src, uint32 W, uint32 rows, uint32 base, uint32 *dst)
{
	cc_unpack32_neon(src, W, rows, base, dst);
}

void
sift_cc_unpack_range32_neon(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	cc_unpack_range32_neon(src, W, rows, lo, span, bits);
}

void
sift_cc_alp_keys_f4_neon(int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	cc_alp_keys_f4_neon(dst, u, n, dbase, div);
}

void
sift_cc_rle_expand32_neon(const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst)
{
	cc_rle_expand32_neon(vals, lens, nruns, dst);
}

#endif
