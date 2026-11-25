/*
 * agg/kern6/avx2.c - AVX2 operators of the 2048-row engine: bitmap words four at a time, the selection vector from a byte table,
 * the 64-bit hash with the multiply built from 32-bit products (a selection vector is a gather), and the sums split into 32-bit
 * halves so that the lanes never carry (the exact 128-bit sum of 64-bit values is the sum of the low halves, plus the sum of the
 * high halves shifted by 32, minus 2^64 for each negative value). The predicates are in agg/kern6/avx2_cmp.c, the minimums,
 * maximums and arithmetic in agg/kern6/avx2_agg.c, the grouped accumulation (AVX2 has no scatter) in agg/kern6/gid.h. Results are
 * bit-identical to the scalar table (agg/kern6/kern6.c).
 */
#include "core/host.h"

#include "agg/kern6/avx2.h"

#if defined(__x86_64__)

/* The largest group of rows [from, n) of a batch without a selection vector, eight at a time (the probe of the four banks of agg/kern6/gid.h) */
K2 uint32
gid_max_nosel(const uint32 *gid, int from, int n)
{
	__m256i		m = _mm256_setzero_si256();
	uint32		l[8],
				r = 0;
	int			i = from;

	for (; i + 8 <= n; i += 8)
		m = _mm256_max_epu32(m, LD(gid + i));
	ST(l, m);
	for (int k = 0; k < 8; k++)
		r = Max(r, l[k]);
	for (; i < n; i++)
		r = Max(r, gid[i]);
	return r;
}

#define AK6_GID_ATTR		SIFT_TARGET_AVX2
#define AK6_GID_MAX_NOSEL	gid_max_nosel
#include "agg/kern6/gid.h"

/* ---- bitmaps ---- */

F2 uint16
mask_andn_del_avx2(uint64 *dst, const uint64 *del, uint16 n)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 4 <= full; w += 4)
	{
		__m256i		x = _mm256_andnot_si256(LD(del + w), LD(dst + w));

		ST(dst + w, x);
		pc += pc4(x);
	}
	for (; w < full; w++)
	{
		dst[w] &= ~del[w];
		pc += _mm_popcnt_u64(dst[w]);
	}
	if (w < nw)
	{
		dst[w] = dst[w] & ~del[w] & AK6_TAIL(n);
		pc += _mm_popcnt_u64(dst[w]);
	}
	return pc;
}

F2 uint16
mask_and_avx2(uint64 *dst, const uint64 *a, const uint64 *b, uint16 n)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 4 <= full; w += 4)
	{
		__m256i		x = _mm256_and_si256(LD(a + w), LD(b + w));

		ST(dst + w, x);
		pc += pc4(x);
	}
	for (; w < full; w++)
	{
		dst[w] = a[w] & b[w];
		pc += _mm_popcnt_u64(dst[w]);
	}
	if (w < nw)
	{
		dst[w] = a[w] & b[w] & AK6_TAIL(n);
		pc += _mm_popcnt_u64(dst[w]);
	}
	return pc;
}

/* The indexes of the set bits of one word: a run of 64, the few bits one by one, or a byte table eight indexes at a time */
K2 uint32
word_to_sel(uint16 *sel, uint32 k, uint64 bits, uint32 base)
{
	if (bits == ~UINT64CONST(0))
	{
		const __m128i iota = _mm_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7);

		for (int j = 0; j < 8; j++)
			_mm_storeu_si128((__m128i *) (sel + k + 8 * j), _mm_add_epi16(iota, _mm_set1_epi16((short) (base + 8 * j))));
		return k + 64;
	}
	if (_mm_popcnt_u64(bits) <= 4)
	{
		for (; bits; bits &= bits - 1)
			sel[k++] = (uint16) (base + _tzcnt_u64(bits));
		return k;
	}
	for (int j = 0; j < 8; j++)
	{
		const uint32 b = (uint32) (bits >> (8 * j)) & 0xFF;
		__m128i		idx = _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *) ak6_byte_idx[b]));

		_mm_storeu_si128((__m128i *) (sel + k), _mm_add_epi16(idx, _mm_set1_epi16((short) (base + 8 * j))));
		k += _mm_popcnt_u32(b);
	}
	return k;
}

F2 uint16
mask_to_sel16_avx2(uint16 *sel, const uint64 *mask, uint16 n)
{
	const int	nw = AK6_NW(n);
	uint32		k = 0;

	for (int w = 0; w < nw; w++)
	{
		uint64		bits = mask[w] & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0));

		if (bits)
			k = word_to_sel(sel, k, bits, w * 64);
	}
	return k;
}

/* dst &= kp, or dst &= (~kp & doc_valid), over n bits */
F2 uint16
key_present_apply_avx2(uint64 *dst, const uint64 *kp, const uint64 *doc_valid, uint16 n, bool negate)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 4 <= full; w += 4)
	{
		__m256i		x = _mm256_and_si256(LD(dst + w), negate ? _mm256_andnot_si256(LD(kp + w), LD(doc_valid + w)) : LD(kp + w));

		ST(dst + w, x);
		pc += pc4(x);
	}
	for (; w < nw; w++)
	{
		dst[w] &= negate ? ~kp[w] & doc_valid[w] : kp[w];
		if (w == nw - 1)
			dst[w] &= AK6_TAIL(n);
		pc += _mm_popcnt_u64(dst[w]);
	}
	return pc;
}

/* ---- hashing ---- */

F2 void
hash_i64_avx2(uint64 *h, const int64 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 4 <= n; i += 4)
		ST(h + i, hash_step(mix ? LD(h + i) : _mm256_setzero_si256(), ld64x4(v, sel, i), mix));
	for (; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) v[ROW_OF_SEL(sel, i)], mix);
}

F2 void
hash_i32_avx2(uint64 *h, const int32 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 4 <= n; i += 4)
		ST(h + i, hash_step(mix ? LD(h + i) : _mm256_setzero_si256(), ld32x4(v, sel, i), mix));
	for (; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) (int64) v[ROW_OF_SEL(sel, i)], mix);
}

F2 void
hash_i128_avx2(uint64 *h, const SiftI128 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m256i		lo,
					hi;

		ld128x4(v, sel, i, &lo, &hi);
		ST(h + i, hash_step(hash_step(mix ? LD(h + i) : _mm256_setzero_si256(), lo, mix), hi, true));
	}
	for (; i < n; i++)
	{
		const SiftI128 *x = &v[ROW_OF_SEL(sel, i)];

		h[i] = AK6_HASH_STEP(AK6_HASH_STEP(h[i], x->lo, mix), (uint64) x->hi, true);
	}
}

/* A view of at most 12 bytes hashes by its two halves with no dereference; the lanes of a longer one are the scalar function of the row */
F2 void
hash_strview_avx2(uint64 *h, const SiftStrView *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m256i		hdr,
					tail,
					r;
		uint32		longs;

		ld128x4(v, sel, i, &hdr, &tail);
		longs = (uint32) _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(_mm256_and_si256(hdr, LO32), SET64(12))));
		r = hash_step(hash_step(mix ? LD(h + i) : _mm256_setzero_si256(), hdr, mix), tail, true);
		if (__builtin_popcount(longs) > 1)
		{
			for (int j = i; j < i + 4; j++)
				h[j] = ak6_sv_hash(h[j], &v[ROW_OF_SEL(sel, j)], mix);
		}
		else if (longs)
		{
			uint64		lane[4];

			ST(lane, r);
			for (int j = 0; j < 4; j++)
				if ((longs >> j) & 1)
					lane[j] = ak6_sv_hash(h[i + j], &v[ROW_OF_SEL(sel, i + j)], mix);
			memcpy(h + i, lane, sizeof(lane));
		}
		else
			ST(h + i, r);
	}
	for (; i < n; i++)
		h[i] = ak6_sv_hash(h[i], &v[ROW_OF_SEL(sel, i)], mix);
}

/* ---- reductions ---- */

F2 int64
sum_i32_avx2(const int32 *v, const uint64 *mask, uint16 n)
{
	__m256i		acc = _mm256_setzero_si256();
	int64		s = 0,
				l[4];
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m256i		x = LD(v + i);

		if (mask)
			x = _mm256_and_si256(x, lanes32(BITS_AT(mask, i, 0xFF)));
		acc = _mm256_add_epi64(acc, _mm256_add_epi64(_mm256_cvtepi32_epi64(_mm256_castsi256_si128(x)),
													 _mm256_cvtepi32_epi64(_mm256_extracti128_si256(x, 1))));
	}
	ST(l, acc);
	s = l[0] + l[1] + l[2] + l[3];
	for (; i < n; i++)
		if (!mask || ((mask[i >> 6] >> (i & 63)) & 1))
			s += v[i];
	return s;
}

F2 SiftI128
sum_i64_to_i128_avx2(const int64 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	__m256i		a = _mm256_setzero_si256(),
				b = a,
				c = a;
	uint64		la[4],
				lb[4],
				lc[4];
	ak6_u128	s = ak6_u128_of(init);
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m256i		x = LD(v + i),
					u;

		if (mask)
			x = _mm256_and_si256(x, lanes64(BITS_AT(mask, i, 0xF)));
		u = _mm256_srli_epi64(x, 32);
		a = _mm256_add_epi64(a, _mm256_and_si256(x, LO32));
		b = _mm256_add_epi64(b, u);
		c = _mm256_add_epi64(c, _mm256_srli_epi64(u, 31));
	}
	ST(la, a);
	ST(lb, b);
	ST(lc, c);
	s += ak6_split_total(la[0] + la[1] + la[2] + la[3], lb[0] + lb[1] + lb[2] + lb[3], lc[0] + lc[1] + lc[2] + lc[3]);
	for (; i < n; i++)
		if (!mask || ((mask[i >> 6] >> (i & 63)) & 1))
			s += (ak6_u128) (__int128) v[i];
	return ak6_i128_of(s);
}

F2 bool
sum_i64_checked_avx2(const int64 *v, const uint64 *mask, uint16 n, int64 *out)
{
	const SiftI128 zero = {0, 0};

	return ak6_fits_i64(sum_i64_to_i128_avx2(v, mask, n, zero), out);
}

/* Two 128-bit values per register: the lanes of a value are the pair of lanes of one row bit */
F2 SiftI128
sum_i128_avx2(const SiftI128 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	static const uint8 pair[4] = {0x0, 0x3, 0xC, 0xF};
	__m256i		a = _mm256_setzero_si256(),
				b = a;
	uint64		la[4],
				lb[4];
	ak6_u128	s = ak6_u128_of(init);
	int			i = 0;

	for (; i + 2 <= n; i += 2)
	{
		__m256i		x = LD(v + i);

		if (mask)
			x = _mm256_and_si256(x, lanes64(pair[BITS_AT(mask, i, 3)]));
		a = _mm256_add_epi64(a, _mm256_and_si256(x, LO32));
		b = _mm256_add_epi64(b, _mm256_srli_epi64(x, 32));
	}
	ST(la, a);
	ST(lb, b);
	s += (ak6_u128) (la[0] + ((ak6_u128) lb[0] << 32)) + (la[2] + ((ak6_u128) lb[2] << 32));
	s += (ak6_u128) (uint64) (la[1] + (lb[1] << 32) + la[3] + (lb[3] << 32)) << 64;
	if (i < n && (!mask || ((mask[i >> 6] >> (i & 63)) & 1)))
		s += ak6_u128_of(v[i]);
	return ak6_i128_of(s);
}

F2 double
sum_f64_fast_avx2(const double *v, const uint64 *mask, uint16 n, double init)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m256d		x0 = LDPD(v + i),
					x1 = LDPD(v + i + 4);

		if (mask)
		{
			const uint32 m = BITS_AT(mask, i, 0xFF);

			x0 = _mm256_and_pd(x0, _mm256_castsi256_pd(lanes64(m & 15)));
			x1 = _mm256_and_pd(x1, _mm256_castsi256_pd(lanes64(m >> 4)));
		}
		a0 = _mm256_add_pd(a0, x0);
		a1 = _mm256_add_pd(a1, x1);
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	for (; i < n; i++)
		a[i & 7] += (!mask || ((mask[i >> 6] >> (i & 63)) & 1)) ? v[i] : 0.0;
	return ak6_f64_tree(a, init);
}

#define AK6_INIT2(s)	.s = s##_avx2,
#define AK6_INIT2X(s)	AK6_INIT_AT(s, avx2)

const SiftAggKernelsV6 sift_aggkern6_avx2 = {
	AK6_INIT2(mask_andn_del) AK6_INIT2(mask_and) AK6_INIT2(mask_to_sel16) AK6_INIT2(key_present_apply)
	AK6_INIT2(hash_i32) AK6_INIT2(hash_i64) AK6_INIT2(hash_i128) AK6_INIT2(hash_strview)
	AK6_INIT2(sum_i32) AK6_INIT2(sum_i64_checked) AK6_INIT2(sum_i64_to_i128) AK6_INIT2(sum_i128) AK6_INIT2(sum_f64_fast)
	AK6_OTHER_AVX2(AK6_INIT2X)
	AK6_FMV_SLOTS(AK6_INIT_FMV)
	AK6_ADDED_SLOTS(AK6_INIT2X)
	.count_gid = ak6_g_count_gid,
	.sum_gid_i32 = ak6_g_sum_gid_i32,
	.sum_gid_i64 = ak6_g_sum_gid_i64,
	.sum_gid_i64_to_i128 = ak6_g_sum_gid_i64_to_i128,
	.sum_gid_i128 = ak6_g_sum_gid_i128,
	.sum_gid_f64_fast = ak6_g_sum_gid_f64_fast,
	.minmax_gid_i64 = ak6_g_minmax_gid_i64,
	.minmax_gid_f64 = ak6_g_minmax_gid_f64,
};

#endif							/* __x86_64__ */
