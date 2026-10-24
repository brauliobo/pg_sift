/*
 * col/shred_kern_neon.c - AArch64 Advanced SIMD kernels of the shredder output; the results equal col/shred_kern.c's.
 */
#include "core/host.h"

#include <math.h>

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

static inline uint64
state_word(const uint8 *p, uint8x16_t s)
{
	return sift_neon_movemask64(vceqq_u8(vld1q_u8(p), s), vceqq_u8(vld1q_u8(p + 16), s), vceqq_u8(vld1q_u8(p + 32), s), vceqq_u8(vld1q_u8(p + 48), s));
}

void
sift_shred_state_bits_neon(const uint8 *state, uint32 n, uint64 *nulls, uint64 *ovf)
{
	uint32		i = 0;

	for (; i + 64 <= n; i += 64)
	{
		nulls[i / 64] = ~state_word(state + i, vdupq_n_u8(SIFT_SHRED_VALUE));
		ovf[i / 64] = state_word(state + i, vdupq_n_u8(SIFT_SHRED_OVERFLOW));
	}
	if (i < n)
	{
		uint64		nw = 0,
					ow = 0;

		for (uint32 j = 0; i + j < n; j++)
		{
			nw |= (uint64) (state[i + j] != SIFT_SHRED_VALUE) << j;
			ow |= (uint64) (state[i + j] == SIFT_SHRED_OVERFLOW) << j;
		}
		nulls[i / 64] = nw;
		ovf[i / 64] = ow;
	}
}

/* The two state bytes at p as two 64-bit lanes that are all ones where the state is VALUE */
static inline uint64x2_t
value_lanes(const uint8 *p)
{
	uint16		w;

	memcpy(&w, p, 2);
	return vceqq_u64(vmovl_u32(vget_low_u32(vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(w)))))), vdupq_n_u64(SIFT_SHRED_VALUE));
}

bool
sift_shred_minmax_i64_neon(const uint8 *state, const int64 *val, uint32 n, int64 *min, int64 *max)
{
	int64x2_t	mn = vdupq_n_s64(PG_INT64_MAX),
				mx = vdupq_n_s64(PG_INT64_MIN);
	uint64x2_t	any = vdupq_n_u64(0);
	uint32		i = 0;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	bool		found;

	for (; i + 2 <= n; i += 2)
	{
		const uint64x2_t m = value_lanes(state + i);
		const int64x2_t v = vld1q_s64(val + i),
					a = vbslq_s64(m, v, vdupq_n_s64(PG_INT64_MAX)),
					b = vbslq_s64(m, v, vdupq_n_s64(PG_INT64_MIN));

		mn = vbslq_s64(vcltq_s64(a, mn), a, mn);
		mx = vbslq_s64(vcgtq_s64(b, mx), b, mx);
		any = vorrq_u64(any, m);
	}
	found = (vgetq_lane_u64(any, 0) | vgetq_lane_u64(any, 1)) != 0;
	lo = Min(vgetq_lane_s64(mn, 0), vgetq_lane_s64(mn, 1));
	hi = Max(vgetq_lane_s64(mx, 0), vgetq_lane_s64(mx, 1));
	for (; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		lo = Min(lo, val[i]);
		hi = Max(hi, val[i]);
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

bool
sift_shred_minmax_f64_neon(const uint8 *state, const double *val, uint32 n, double *min, double *max)
{
	float64x2_t mn = vdupq_n_f64(INFINITY),
				mx = vdupq_n_f64(-INFINITY);
	uint64x2_t	any = vdupq_n_u64(0);
	uint32		i = 0;
	double		lo,
				hi;
	bool		found;

	for (; i + 2 <= n; i += 2)
	{
		const uint64x2_t m = value_lanes(state + i);
		const float64x2_t v = vld1q_f64(val + i);

		mn = vminq_f64(mn, vbslq_f64(m, v, vdupq_n_f64(INFINITY)));
		mx = vmaxq_f64(mx, vbslq_f64(m, v, vdupq_n_f64(-INFINITY)));
		any = vorrq_u64(any, m);
	}
	found = (vgetq_lane_u64(any, 0) | vgetq_lane_u64(any, 1)) != 0;
	lo = Min(vgetq_lane_f64(mn, 0), vgetq_lane_f64(mn, 1));
	hi = Max(vgetq_lane_f64(mx, 0), vgetq_lane_f64(mx, 1));
	for (; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		lo = Min(lo, val[i]);
		hi = Max(hi, val[i]);
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

#endif
