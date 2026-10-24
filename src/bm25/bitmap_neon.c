/*
 * AArch64 Advanced SIMD bitmap kernels: the generic template, and the population count of 64 bytes and more with CNT. tools/codec builds this file
 * without the server's headers: it tests the target as bitmap.c does and does not include core/neon.h.
 */
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>

#define KFN(x) x##_neon
#define and_wide_neon sift_and_wide_gen
#define or_wide_neon sift_or_wide_gen
#define andnot_wide_neon sift_andnot_wide_gen
#define cnt_wide_neon sift_cnt_wide_gen
#include "bm25/bitmap_kern.h"

#define SIFT_KERN_TMPL
#include "bm25/bitmap_kern.h"

/* CNT per byte; four registers are added as bytes (at most 32 a lane) before they are widened, and the 16 bit lanes are flushed in time */
static uint64_t popcount_neon_cnt(const uint8_t *p, size_t n)
{
	uint64x2_t acc = vdupq_n_u64(0);
	size_t i = 0;

	while (i + 64 <= n) {
		uint16x8_t a16 = vdupq_n_u16(0);

		/* a 16 bit lane takes at most 2 * 32 a step: 1000 steps stay below 65536 */
		for (size_t e = i + 64 * 1000 < n ? i + 64 * 1000 : n; i + 64 <= e; i += 64) {
			uint8x16_t c = vaddq_u8(vaddq_u8(vcntq_u8(vld1q_u8(p + i)), vcntq_u8(vld1q_u8(p + i + 16))),
						vaddq_u8(vcntq_u8(vld1q_u8(p + i + 32)), vcntq_u8(vld1q_u8(p + i + 48))));

			a16 = vpadalq_u8(a16, c);
		}
		acc = vpadalq_u32(acc, vpaddlq_u16(a16));
	}
	return vaddvq_u64(acc) + popcount_neon(p + i, n - i);
}

static uint64_t popcount_neon_dispatch(const uint8_t *p, size_t n) { return n >= 64 ? popcount_neon_cnt(p, n) : popcount_neon(p, n); }

const sift_kernels sift_kern_neon = { popcount_neon_dispatch, and2_neon, or2_neon, andnot_neon, and_count_neon,
				      count_mask_neon, tids_neon, ranks_neon, vop_neon, bm25_neon };
#endif
