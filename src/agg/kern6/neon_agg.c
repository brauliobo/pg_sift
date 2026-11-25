/*
 * agg/kern6/neon_agg.c - the AArch64 Advanced SIMD minimums and maximums and numeric programs of the 2048-row engine. The rows a mask
 * leaves out are replaced by the identity of the reduction so that the lanes need no branch. A double is reduced as its float8 key
 * (agg/kern6/int.h), a signed integer with the order of the doubles, and turned back at the end. A 128-bit value is one register, its
 * lanes (low, high): it is greater when its high half is greater (signed) or equal and its low half is greater (unsigned), and the
 * result of the compare picks the register to keep. Results are bit-identical to the scalar table.
 */
#include "core/host.h"

#include "agg/kern6/neon.h"

#if defined(SIFT_HAVE_NEON)

bool
ak6_minmax_i32_neon(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, uint16 n)
{
	int32x4_t	lo = vdupq_n_s32(INT32_MAX),
				hi = vdupq_n_s32(INT32_MIN);
	int32		a,
				b;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 4 <= n; i += 4)
	{
		const int32x4_t x = vld1q_s32(v + i);
		const uint32x4_t m = mask ? lanes4(BITS_AT(mask, i, 0xF)) : vdupq_n_u32(~0u);

		lo = vminq_s32(lo, vbslq_s32(m, x, vdupq_n_s32(INT32_MAX)));
		hi = vmaxq_s32(hi, vbslq_s32(m, x, vdupq_n_s32(INT32_MIN)));
	}
	a = vminvq_s32(lo);
	b = vmaxvq_s32(hi);
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
		{
			a = Min(a, v[i]);
			b = Max(b, v[i]);
		}
	*mn = a;
	*mx = b;
	return true;
}

/* The minimum and maximum of the int64 values, or of the float8 keys of the doubles (f8) */
NK bool
minmax64(int64 *mn, int64 *mx, const void *v, const uint64 *mask, uint16 n, bool f8)
{
	int64x2_t	lo = vdupq_n_s64(INT64_MAX),
				hi = vdupq_n_s64(INT64_MIN);
	int64		a,
				b;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 2 <= n; i += 2)
	{
		const int64x2_t x = f8 ? f8key2(vld1q_f64((const double *) v + i)) : vld1q_s64((const int64 *) v + i);
		const uint64x2_t m = mask ? lanes2(BITS_AT(mask, i, 3)) : vdupq_n_u64(~UINT64CONST(0));
		const int64x2_t xl = vbslq_s64(m, x, vdupq_n_s64(INT64_MAX)),
					xh = vbslq_s64(m, x, vdupq_n_s64(INT64_MIN));

		lo = vbslq_s64(vcltq_s64(xl, lo), xl, lo);
		hi = vbslq_s64(vcgtq_s64(xh, hi), xh, hi);
	}
	a = Min(vgetq_lane_s64(lo, 0), vgetq_lane_s64(lo, 1));
	b = Max(vgetq_lane_s64(hi, 0), vgetq_lane_s64(hi, 1));
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
		{
			const int64 x = f8 ? ak6_f8_key(((const double *) v)[i]) : ((const int64 *) v)[i];

			a = Min(a, x);
			b = Max(b, x);
		}
	*mn = a;
	*mx = b;
	return true;
}

bool
ak6_minmax_i64_neon(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, uint16 n)
{
	return minmax64(mn, mx, v, mask, n, false);
}

bool
ak6_minmax_f64_neon(double *mn, double *mx, const double *v, const uint64 *mask, uint16 n)
{
	int64		a,
				b;

	if (!minmax64(&a, &b, v, mask, n, true))
		return false;
	*mn = ak6_f8_of_key(a);
	*mx = ak6_f8_of_key(b);
	return true;
}

/* x > y as the lane mask of the whole pair */
NK uint64x2_t
gt128(uint64x2_t x, uint64x2_t y)
{
	const uint64x2_t gts = vcgtq_s64(vreinterpretq_s64_u64(x), vreinterpretq_s64_u64(y)),
				gtu = vcgtq_u64(x, y),
				eq = vceqq_u64(x, y);

	return vdupq_n_u64(vgetq_lane_u64(gts, 1) | (vgetq_lane_u64(eq, 1) & vgetq_lane_u64(gtu, 0)));
}

/* One value a register, two rows an iteration (the minimum and the maximum are two chains); the masked-out rows are skipped */
bool
ak6_minmax_i128_neon(SiftI128 *mn, SiftI128 *mx, const SiftI128 *v, const uint64 *mask, uint16 n)
{
	uint64x2_t	lo = vdupq_n_u64(0),
				hi = vdupq_n_u64(0);
	bool		any = false;

	for (int i = 0; i < n; i++)
	{
		uint64x2_t	x;

		if (mask && !ROWBIT(mask, i))
			continue;
		x = vld1q_u64((const uint64 *) (v + i));
		if (!any)
			lo = hi = x;
		lo = vbslq_u64(gt128(lo, x), x, lo);
		hi = vbslq_u64(gt128(x, hi), x, hi);
		any = true;
	}
	if (!any)
		return false;
	vst1q_u64((uint64 *) mn, lo);
	vst1q_u64((uint64 *) mx, hi);
	return true;
}

void
ak6_lin_i64_neon(int64 *dst, const int64 *a, int32 ka, const int64 *b, int32 kb, uint16 n)
{
	const uint64x2_t va = vdupq_n_u64((uint64) (int64) ka),
				vb = vdupq_n_u64((uint64) (int64) kb);
	int			i = 0;

	for (; i + 2 <= n; i += 2)
		vst1q_u64((uint64 *) (dst + i),
				  vaddq_u64(neon_mul64(vld1q_u64((const uint64 *) (a + i)), va), neon_mul64(vld1q_u64((const uint64 *) (b + i)), vb)));
	for (; i < n; i++)
		dst[i] = (int64) ((uint64) a[i] * (uint64) (int64) ka + (uint64) b[i] * (uint64) (int64) kb);
}

void
ak6_mulw_i64_neon(int64 *dst, const int64 *a, const int64 *b, uint16 n)
{
	int			i = 0;

	for (; i + 2 <= n; i += 2)
		vst1q_u64((uint64 *) (dst + i), neon_mul64(vld1q_u64((const uint64 *) (a + i)), vld1q_u64((const uint64 *) (b + i))));
	for (; i < n; i++)
		dst[i] = (int64) ((uint64) a[i] * (uint64) b[i]);
}

#endif							/* SIFT_HAVE_NEON */
