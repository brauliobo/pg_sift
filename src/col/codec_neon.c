/*
 * col/codec_neon.c - AArch64 Advanced SIMD kernels of the chunk codec, the width and the compare-to-bits primitive of col/codec_vec.h.
 *
 * Only the slots that win on the target CPU (arm64, ns for 4096 values, scalar -> NEON): unpack32 (W 3..20) 1500 -> 195..250, unpack_range32
 * 1750..2125 -> 312..375, rle_expand32 1125 -> 273, alp_keys_f4 1375 -> 719. The others stay the scalar functions because the compiler already
 * vectorizes them or the NEON loop is slower: max32 and prefix32 tie (105 and 1313 ns), scale32 117 -> 234, scale64 375 -> 688, prefix64 1813 -> 2625,
 * alp_keys_f8 1000 -> 1563, alp_f8 516 -> 1000, alp_f4 438 -> 516.
 */
#include "core/host.h"

#include "col/codec_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

/* the 32 bits of eight u32 compares: narrowed to bytes and folded, not one add across per vector */
static inline uint32
cc_rowmask_neon(const uint32x4_t *v, uint32x4_t span)
{
	uint8x16_t	z = vdupq_n_u8(0);

	return (uint32) sift_neon_movemask64(sift_neon_pack16(sift_neon_pack32(vcleq_u32(v[0], span), vcleq_u32(v[1], span)), sift_neon_pack32(vcleq_u32(v[2], span), vcleq_u32(v[3], span))),
										 sift_neon_pack16(sift_neon_pack32(vcleq_u32(v[4], span), vcleq_u32(v[5], span)), sift_neon_pack32(vcleq_u32(v[6], span), vcleq_u32(v[7], span))), z, z);
}

#define CC_VB 16
#define CC_FN(x) cc_##x##_neon
#define CC_ROWMASK(v, span) cc_rowmask_neon((const uint32x4_t *) (v), (uint32x4_t) (span))
#include "col/codec_vec.h"

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
