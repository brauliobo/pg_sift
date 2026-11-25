/*
 * agg/kern6/avx2_agg.c - the AVX2 minimums and maximums and the numeric programs of the 2048-row engine. The rows a mask leaves out are
 * replaced by the identity of the reduction (the largest value for a minimum, the smallest for a maximum) so that the lanes need no
 * branch; AVX2 has no 64-bit minimum, so the compare and the blend stand for it. A double is reduced as its float8 key (agg/kern6/int.h),
 * a signed integer with the order of the doubles, and turned back at the end. A 128-bit value is compared as in agg/kern6/avx2_cmp.c.
 * Results are bit-identical to the scalar table.
 */
#include "core/host.h"

#include "agg/kern6/avx2.h"

#if defined(__x86_64__)

#define MIN64(a, b)		_mm256_blendv_epi8(b, a, _mm256_cmpgt_epi64(b, a))
#define MAX64(a, b)		_mm256_blendv_epi8(b, a, _mm256_cmpgt_epi64(a, b))

X2 bool
ak6_minmax_i32_avx2(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, uint16 n)
{
	__m256i		lo = SET32(INT32_MAX),
				hi = SET32(INT32_MIN);
	int32		l[8],
				h[8],
				a = INT32_MAX,
				b = INT32_MIN;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 8 <= n; i += 8)
	{
		const __m256i x = LD(v + i),
					m = mask ? lanes32(BITS_AT(mask, i, 0xFF)) : _mm256_set1_epi32(-1);

		lo = _mm256_min_epi32(lo, _mm256_blendv_epi8(SET32(INT32_MAX), x, m));
		hi = _mm256_max_epi32(hi, _mm256_blendv_epi8(SET32(INT32_MIN), x, m));
	}
	ST(l, lo);
	ST(h, hi);
	for (int k = 0; k < 8; k++)
	{
		a = Min(a, l[k]);
		b = Max(b, h[k]);
	}
	for (; i < n; i++)
		if (!mask || AK6_ROWBIT(mask, i))
		{
			a = Min(a, v[i]);
			b = Max(b, v[i]);
		}
	*mn = a;
	*mx = b;
	return true;
}

/* The minimum and maximum of the int64 values, or of the float8 keys of the doubles (f8) */
K2 bool
minmax64(int64 *mn, int64 *mx, const void *v, const uint64 *mask, uint16 n, bool f8)
{
	__m256i		lo = SET64(INT64_MAX),
				hi = SET64(INT64_MIN);
	int64		l[4],
				h[4],
				a = INT64_MAX,
				b = INT64_MIN;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 4 <= n; i += 4)
	{
		const __m256i x = f8 ? f8key4(LDPD((const double *) v + i)) : LD((const int64 *) v + i),
					m = mask ? lanes64(BITS_AT(mask, i, 0xF)) : _mm256_set1_epi64x(-1);

		lo = MIN64(lo, _mm256_blendv_epi8(SET64(INT64_MAX), x, m));
		hi = MAX64(hi, _mm256_blendv_epi8(SET64(INT64_MIN), x, m));
	}
	ST(l, lo);
	ST(h, hi);
	for (int k = 0; k < 4; k++)
	{
		a = Min(a, l[k]);
		b = Max(b, h[k]);
	}
	for (; i < n; i++)
		if (!mask || AK6_ROWBIT(mask, i))
		{
			const int64 x = f8 ? ak6_f8_key(((const double *) v)[i]) : ((const int64 *) v)[i];

			a = Min(a, x);
			b = Max(b, x);
		}
	*mn = a;
	*mx = b;
	return true;
}

X2 bool
ak6_minmax_i64_avx2(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, uint16 n)
{
	return minmax64(mn, mx, v, mask, n, false);
}

X2 bool
ak6_minmax_f64_avx2(double *mn, double *mx, const double *v, const uint64 *mask, uint16 n)
{
	int64		a,
				b;

	if (!minmax64(&a, &b, v, mask, n, true))
		return false;
	*mn = ak6_f8_of_key(a);
	*mx = ak6_f8_of_key(b);
	return true;
}

/*
 * The order of a 128-bit value is its high half (signed), then its low half (unsigned): two passes over the rows (hot in the cache). The first
 * finds the extreme high halves, the second the extreme low half of the rows that have them. The halves of four values are an unpack and a permute
 * of two loads; the row mask selects the lanes of each reduction (the others are the identity). The unsigned low halves are compared as signed with
 * the sign bit flipped.
 */
K2 void
halves128(const SiftI128 *v, __m256i *lo, __m256i *hi)
{
	const __m256i a = LD(v),
				b = LD(v + 2);

	*lo = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(a, b), 0xD8);
	*hi = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(a, b), 0xD8);
}

X2 bool
ak6_minmax_i128_avx2(SiftI128 *mn, SiftI128 *mx, const SiftI128 *v, const uint64 *mask, uint16 n)
{
	const __m256i all = _mm256_set1_epi64x(-1),
				sign = SET64(SIGN64);
	__m256i		hmn = SET64(INT64_MAX),
				hmx = SET64(INT64_MIN),
				lmn = SET64(INT64_MAX),
				lmx = SET64(INT64_MIN);
	int64		hl[4],
				hh[4],
				ll[4],
				lh[4],
				hlo = INT64_MAX,
				hhi = INT64_MIN,
				llo = INT64_MAX,
				lhi = INT64_MIN;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 4 <= n; i += 4)
	{
		__m256i		lo,
					hi;
		const __m256i m = mask ? lanes64(BITS_AT(mask, i, 0xF)) : all;

		halves128(v + i, &lo, &hi);
		hmn = MIN64(hmn, _mm256_blendv_epi8(SET64(INT64_MAX), hi, m));
		hmx = MAX64(hmx, _mm256_blendv_epi8(SET64(INT64_MIN), hi, m));
	}
	ST(hl, hmn);
	ST(hh, hmx);
	for (int k = 0; k < 4; k++)
	{
		hlo = Min(hlo, hl[k]);
		hhi = Max(hhi, hh[k]);
	}
	for (int r = i; r < n; r++)
		if (!mask || AK6_ROWBIT(mask, r))
		{
			hlo = Min(hlo, v[r].hi);
			hhi = Max(hhi, v[r].hi);
		}
	{
		const __m256i vlo = SET64(hlo),
					vhi = SET64(hhi);

		for (i = 0; i + 4 <= n; i += 4)
		{
			__m256i		lo,
						hi;
			const __m256i m = mask ? lanes64(BITS_AT(mask, i, 0xF)) : all;

			halves128(v + i, &lo, &hi);
			lo = _mm256_xor_si256(lo, sign);
			lmn = MIN64(lmn, _mm256_blendv_epi8(SET64(INT64_MAX), lo, _mm256_and_si256(m, _mm256_cmpeq_epi64(hi, vlo))));
			lmx = MAX64(lmx, _mm256_blendv_epi8(SET64(INT64_MIN), lo, _mm256_and_si256(m, _mm256_cmpeq_epi64(hi, vhi))));
		}
	}
	ST(ll, lmn);
	ST(lh, lmx);
	for (int k = 0; k < 4; k++)
	{
		llo = Min(llo, ll[k]);
		lhi = Max(lhi, lh[k]);
	}
	{
		uint64		ulo = (uint64) llo ^ (uint64) SIGN64,
					uhi = (uint64) lhi ^ (uint64) SIGN64;

		for (int r = i; r < n; r++)
			if (!mask || AK6_ROWBIT(mask, r))
			{
				if (v[r].hi == hlo)
					ulo = Min(ulo, v[r].lo);
				if (v[r].hi == hhi)
					uhi = Max(uhi, v[r].lo);
			}
		mn->lo = ulo;
		mn->hi = hlo;
		mx->lo = uhi;
		mx->hi = hhi;
	}
	return true;
}

#endif							/* __x86_64__ */
