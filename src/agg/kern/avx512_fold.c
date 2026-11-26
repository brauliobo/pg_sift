/*
 * agg/kern/avx512_fold.c - the AVX-512 minimums and maximums, the grouped accumulation and the numeric programs of the 2048-row engine.
 *
 * The reductions update the accumulator under the row mask (VPMINSQ and VPMAXSQ in the mask register of the rows); a double is reduced as
 * its float8 key (agg/kern/int.h), a signed integer with the order of the doubles, and turned back at the end.
 *
 * A grouped accumulation takes the groups of eight rows, VPCONFLICTD says whether two of them are one, and if not the cells are
 * gathered, updated and scattered at once: every cell is touched by one row, so the result is that of the rows in order, a float sum
 * included. A window with a repeated group runs its eight rows in order, one after the other.
 * Results are bit-identical to the scalar table.
 */
#include "core/host.h"

#include "agg/kern/avx512.h"

#if defined(__x86_64__)

X5 bool
ak6_minmax_i32_avx512(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, uint16 n)
{
	__m512i		lo = SET32(INT32_MAX),
				hi = SET32(INT32_MIN);
	int32		a = INT32_MAX,
				b = INT32_MIN;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 16 <= n; i += 16)
	{
		const __m512i x = LD(v + i);
		const __mmask16 m = mask ? (__mmask16) BITS_AT(mask, i, 0xFFFF) : (__mmask16) 0xFFFF;

		lo = _mm512_mask_min_epi32(lo, m, lo, x);
		hi = _mm512_mask_max_epi32(hi, m, hi, x);
	}
	a = Min(a, _mm512_reduce_min_epi32(lo));
	b = Max(b, _mm512_reduce_max_epi32(hi));
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
K5 bool
minmax64(int64 *mn, int64 *mx, const void *v, const uint64 *mask, uint16 n, bool f8)
{
	__m512i		lo = SET64(INT64_MAX),
				hi = SET64(INT64_MIN);
	int64		a = INT64_MAX,
				b = INT64_MIN;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 8 <= n; i += 8)
	{
		const __m512i x = f8 ? f8key8(LDPD((const double *) v + i)) : LD((const int64 *) v + i);
		const __mmask8 m = mask ? (__mmask8) BITS_AT(mask, i, 0xFF) : (__mmask8) 0xFF;

		lo = _mm512_mask_min_epi64(lo, m, lo, x);
		hi = _mm512_mask_max_epi64(hi, m, hi, x);
	}
	a = Min(a, _mm512_reduce_min_epi64(lo));
	b = Max(b, _mm512_reduce_max_epi64(hi));
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

X5 bool
ak6_minmax_i64_avx512(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, uint16 n)
{
	return minmax64(mn, mx, v, mask, n, false);
}

X5 bool
ak6_minmax_f64_avx512(double *mn, double *mx, const double *v, const uint64 *mask, uint16 n)
{
	int64		a,
				b;

	if (!minmax64(&a, &b, v, mask, n, true))
		return false;
	*mn = ak6_f8_of_key(a);
	*mx = ak6_f8_of_key(b);
	ak6_f8_zero_sign(mn, mx, v, mask, n);
	return true;
}

/*
 * The order of a 128-bit value is its high half (signed), then its low half (unsigned): two passes over the rows (hot in the cache). The first
 * finds the extreme high halves, the second the extreme low half of the rows that have them. The halves of eight values are two permutes of two
 * loads; the row mask selects the lanes of each reduction.
 */
X5 bool
ak6_minmax_i128_avx512(SiftI128 *mn, SiftI128 *mx, const SiftI128 *v, const uint64 *mask, uint16 n)
{
	const __m512i ilo = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14),
				ihi = _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15);
	__m512i		hmn = SET64(INT64_MAX),
				hmx = SET64(INT64_MIN),
				lmn = SET64(-1),
				lmx = _mm512_setzero_si512();
	int64		hlo = INT64_MAX,
				hhi = INT64_MIN;
	uint64		llo = UINT64_MAX,
				lhi = 0;
	int			i = 0;

	if (!ak6_any_row(mask, n))
		return false;
	for (; i + 8 <= n; i += 8)
	{
		const __m512i a = LD(v + i),
					b = LD(v + i + 4),
					hi = _mm512_permutex2var_epi64(a, ihi, b);
		const __mmask8 k = mask ? (__mmask8) BITS_AT(mask, i, 0xFF) : (__mmask8) 0xFF;

		hmn = _mm512_mask_min_epi64(hmn, k, hmn, hi);
		hmx = _mm512_mask_max_epi64(hmx, k, hmx, hi);
	}
	hlo = Min(hlo, _mm512_reduce_min_epi64(hmn));
	hhi = Max(hhi, _mm512_reduce_max_epi64(hmx));
	for (int r = i; r < n; r++)
		if (!mask || ROWBIT(mask, r))
		{
			hlo = Min(hlo, v[r].hi);
			hhi = Max(hhi, v[r].hi);
		}
	{
		const __m512i vlo = SET64(hlo),
					vhi = SET64(hhi);

		for (i = 0; i + 8 <= n; i += 8)
		{
			const __m512i a = LD(v + i),
						b = LD(v + i + 4),
						lo = _mm512_permutex2var_epi64(a, ilo, b),
						hi = _mm512_permutex2var_epi64(a, ihi, b);
			const __mmask8 k = mask ? (__mmask8) BITS_AT(mask, i, 0xFF) : (__mmask8) 0xFF;

			lmn = _mm512_mask_min_epu64(lmn, k & _mm512_cmpeq_epi64_mask(hi, vlo), lmn, lo);
			lmx = _mm512_mask_max_epu64(lmx, k & _mm512_cmpeq_epi64_mask(hi, vhi), lmx, lo);
		}
	}
	llo = Min(llo, (uint64) _mm512_reduce_min_epu64(lmn));
	lhi = Max(lhi, (uint64) _mm512_reduce_max_epu64(lmx));
	for (int r = i; r < n; r++)
		if (!mask || ROWBIT(mask, r))
		{
			if (v[r].hi == hlo)
				llo = Min(llo, v[r].lo);
			if (v[r].hi == hhi)
				lhi = Max(lhi, v[r].lo);
		}
	mn->lo = llo;
	mn->hi = hlo;
	mx->lo = lhi;
	mx->hi = hhi;
	return true;
}

/* ---- per group accumulation ---- */

/* The groups of the eight active rows i..i+8 */
K5 __m256i
gid8(const uint32 *gid, const uint16 *sel, int i)
{
	return sel ? _mm256_i32gather_epi32((const int *) gid, sel8(sel + i), 4) : _mm256_loadu_si256((const __m256i *) (gid + i));
}

/* No two of the eight groups are the same */
K5 bool
distinct8(__m256i g)
{
	const __m256i c = _mm256_conflict_epi32(g);

	return _mm256_testz_si256(c, c);
}

/*
 * The rows of a grouped slot: FAST(g, i) updates the cells of the groups g of the eight rows from i at once, ROW(r) one row. A window of
 * eight with a repeated group, and the rows after the last window, go row by row in order.
 */
#define GID_ROWS(gid, sel, n, FAST, ROW) \
	do { \
		int i = 0; \
		for (; i + 8 <= (n); i += 8) \
		{ \
			const __m256i g = gid8(gid, sel, i); \
			if (distinct8(g)) \
			{ \
				FAST; \
				continue; \
			} \
			for (int j = i; j < i + 8; j++) \
			{ \
				const int r = ROW_OF_SEL(sel, j); \
				ROW; \
			} \
		} \
		for (; i < (n); i++) \
		{ \
			const int r = ROW_OF_SEL(sel, i); \
			ROW; \
		} \
	} while (0)

X5 void
ak6_count_gid_avx512(int64 *cnt, const uint32 *gid, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 _mm512_i32scatter_epi64(cnt, g, _mm512_add_epi64(_mm512_i32gather_epi64(g, cnt, 8), SET64(1)), 8),
			 cnt[gid[r]]++);
}

X5 void
ak6_sum_gid_i32_avx512(int64 *acc, const uint32 *gid, const int32 *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 _mm512_i32scatter_epi64(acc, g, _mm512_add_epi64(_mm512_i32gather_epi64(g, acc, 8), ld32x8(v, sel, i)), 8),
			 ak6_upd_sum_i32(acc, gid[r], v[r]));
}

X5 void
ak6_sum_gid_i64_avx512(int64 *acc, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 _mm512_i32scatter_epi64(acc, g, _mm512_add_epi64(_mm512_i32gather_epi64(g, acc, 8), ld64x8(v, sel, i)), 8),
			 ak6_upd_sum_i64(acc, gid[r], v[r]));
}

/* acc += (lo, hi) of the cells of the groups g: the carry out of the low halves goes to the high ones */
K5 void
add128_cells(SiftI128 *acc, __m256i g, __m512i vlo, __m512i vhi)
{
	const __m256i idx = _mm256_slli_epi32(g, 1);
	const __m512i alo = _mm512_i32gather_epi64(idx, acc, 8),
				ahi = _mm512_i32gather_epi64(idx, (const long long *) acc + 1, 8),
				lo = _mm512_add_epi64(alo, vlo),
				hs = _mm512_add_epi64(ahi, vhi);
	const __mmask8 carry = _mm512_cmplt_epu64_mask(lo, alo);
	const __m512i hi = _mm512_mask_add_epi64(hs, carry, hs, SET64(1));

	_mm512_i32scatter_epi64(acc, idx, lo, 8);
	_mm512_i32scatter_epi64((long long *) acc + 1, idx, hi, 8);
}

X5 void
ak6_sum_gid_i64_to_i128_avx512(SiftI128 *acc, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 ({ const __m512i x = ld64x8(v, sel, i); add128_cells(acc, g, x, _mm512_srai_epi64(x, 63)); }),
			 ak6_upd_sum_i64_i128(acc, gid[r], v[r]));
}

X5 void
ak6_sum_gid_i128_avx512(SiftI128 *acc, const uint32 *gid, const SiftI128 *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 ({ __m512i lo, hi; ld128x8(v, sel, i, &lo, &hi); add128_cells(acc, g, lo, hi); }),
			 ak6_upd_sum_i128(acc, gid[r], v[r]));
}

X5 void
ak6_sum_gid_f64_fast_avx512(double *acc, const uint32 *gid, const double *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 ({
				 const __m512d s = _mm512_add_pd(_mm512_i32gather_pd(g, acc, 8), _mm512_castsi512_pd(ld64x8((const int64 *) v, sel, i)));

				 _mm512_i32scatter_pd(acc, g, _mm512_mask_mov_pd(s, _mm512_cmp_pd_mask(s, s, _CMP_UNORD_Q), _mm512_castsi512_pd(SET64(AK6_F8_NAN_BITS))), 8);
			 }),
			 acc[gid[r]] = ak6_add_f64(acc[gid[r]], v[r]));
}

X5 void
ak6_minmax_gid_i64_avx512(int64 *mn, int64 *mx, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 ({
				 const __m512i x = ld64x8(v, sel, i);

				 _mm512_i32scatter_epi64(mn, g, _mm512_min_epi64(_mm512_i32gather_epi64(g, mn, 8), x), 8);
				 _mm512_i32scatter_epi64(mx, g, _mm512_max_epi64(_mm512_i32gather_epi64(g, mx, 8), x), 8);
			 }),
			 ak6_upd_minmax_i64(mn, mx, gid[r], v[r]));
}

X5 void
ak6_minmax_gid_f64_avx512(double *mn, double *mx, const uint32 *gid, const double *v, const uint16 *sel, uint16 n)
{
	GID_ROWS(gid, sel, n,
			 ({
				 const __m512i x = f8key8(_mm512_castsi512_pd(ld64x8((const int64 *) v, sel, i)));

				 _mm512_i32scatter_pd(mn, g, f8unkey8(_mm512_min_epi64(f8key8(_mm512_i32gather_pd(g, mn, 8)), x)), 8);
				 _mm512_i32scatter_pd(mx, g, f8unkey8(_mm512_max_epi64(f8key8(_mm512_i32gather_pd(g, mx, 8)), x)), 8);
			 }),
			 ak6_upd_minmax_f64(mn, mx, gid[r], v[r]));
}

#endif							/* __x86_64__ */
