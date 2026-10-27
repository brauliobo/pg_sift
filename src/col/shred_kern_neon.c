/*
 * col/shred_kern_neon.c - AArch64 Advanced SIMD kernels of the shredder output; the results equal col/shred_kern.c's.
 */
#include "core/host.h"

#define SIFT_VEC_NEON
#include "core/simd_isa.h"

#include "col/shred.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

void
sift_shred_state_counts_neon(const uint8 *state, uint32 n, uint32 counts[4])
{
	uint32		c[4] = {0, 0, 0, 0},
				i = 0;

	/* a byte lane counts at most 255 vectors before it wraps */
	while (i + 16 <= n)
	{
		uint8x16_t	a0 = vdupq_n_u8(0),
					a1 = a0,
					a2 = a0,
					a3 = a0;

		for (uint32 e = Min(i + 255 * 16, n & ~15U); i < e; i += 16)
		{
			const uint8x16_t v = vld1q_u8(state + i);

			a0 = vsubq_u8(a0, vceqq_u8(v, vdupq_n_u8(0)));
			a1 = vsubq_u8(a1, vceqq_u8(v, vdupq_n_u8(1)));
			a2 = vsubq_u8(a2, vceqq_u8(v, vdupq_n_u8(2)));
			a3 = vsubq_u8(a3, vceqq_u8(v, vdupq_n_u8(3)));
		}
		c[0] += vaddlvq_u8(a0);
		c[1] += vaddlvq_u8(a1);
		c[2] += vaddlvq_u8(a2);
		c[3] += vaddlvq_u8(a3);
	}
	for (; i < n; i++)
		c[state[i]]++;
	memcpy(counts, c, sizeof(c));
}

/* The 64 states at p that are VALUE and OVERFLOW */
static inline void
sv_words(const uint8 *p, uint64 *value, uint64 *ovf)
{
	const uint8x16_t s2 = vdupq_n_u8(SIFT_SHRED_VALUE),
				s3 = vdupq_n_u8(SIFT_SHRED_OVERFLOW);
	uint8x16_t	a = vld1q_u8(p),
				b = vld1q_u8(p + 16),
				c = vld1q_u8(p + 32),
				d = vld1q_u8(p + 48);

	*value = sift_neon_movemask64(vceqq_u8(a, s2), vceqq_u8(b, s2), vceqq_u8(c, s2), vceqq_u8(d, s2));
	*ovf = sift_neon_movemask64(vceqq_u8(a, s3), vceqq_u8(b, s3), vceqq_u8(c, s3), vceqq_u8(d, s3));
}

/* FMIN where the other level has VMINPD: no NaN reaches it */
#define SV_MIN_F64(a, b) ((sv_d) vminq_f64((float64x2_t) (a), (float64x2_t) (b)))
#define SV_MAX_F64(a, b) ((sv_d) vmaxq_f64((float64x2_t) (a), (float64x2_t) (b)))
#define SV_STORAGE
#define SV_FN(x) sift_shred_##x##_neon
#include "col/shred_vec.h"

#endif
