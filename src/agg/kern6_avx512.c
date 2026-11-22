/*
 * agg/kern6_avx512.c - AVX-512 operators of the 2048-row engine (F, BW, VL, DQ, CD, VPOPCNTDQ, VBMI2): eight bitmap words per
 * instruction with a vector popcount, the selection vector by VPCOMPRESSW (32 rows per instruction), the 64-bit hash with
 * VPMULLQ (a selection vector is a gather), and the sums with the row mask in a mask register (the same carry-free split of the
 * 64-bit values as AVX2). The predicates are in agg/kern6_avx512_cmp.c, the minimums, maximums, arithmetic and the grouped
 * accumulation in agg/kern6_avx512_agg.c. Results are bit-identical to the scalar table (agg/kern6.c).
 */
#include "core/host.h"

#include "agg/kern6_avx512.h"

#if defined(__x86_64__)

/* The largest group of rows [from, n) of a batch without a selection vector, sixteen at a time (the probe of the four banks of agg/kern6_gid.h) */
K5 uint32
gid_max_nosel(const uint32 *gid, int from, int n)
{
	__m512i		m = _mm512_setzero_si512();
	uint32		r;
	int			i = from;

	for (; i + 16 <= n; i += 16)
		m = _mm512_max_epu32(m, LD(gid + i));
	r = _mm512_reduce_max_epu32(m);
	for (; i < n; i++)
		r = Max(r, gid[i]);
	return r;
}

#define AK6_GID_ATTR		SIFT_TARGET_AVX512
#define AK6_GID_MAX_NOSEL	gid_max_nosel
#include "agg/kern6_gid.h"

/* ---- bitmaps ---- */

F5 uint16
mask_andn_del_avx512(uint64 *dst, const uint64 *del, uint16 n)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 8 <= full; w += 8)
	{
		__m512i		x = _mm512_andnot_si512(LD(del + w), LD(dst + w));

		ST(dst + w, x);
		pc += (uint32) _mm512_reduce_add_epi64(_mm512_popcnt_epi64(x));
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

F5 uint16
mask_and_avx512(uint64 *dst, const uint64 *a, const uint64 *b, uint16 n)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 8 <= full; w += 8)
	{
		__m512i		x = _mm512_and_si512(LD(a + w), LD(b + w));

		ST(dst + w, x);
		pc += (uint32) _mm512_reduce_add_epi64(_mm512_popcnt_epi64(x));
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

F5 uint16
mask_to_sel16_avx512(uint16 *sel, const uint64 *mask, uint16 n)
{
	static const uint16 iota[32] __attribute__((aligned(64))) = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
		16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
	const int	nw = AK6_NW(n);
	uint32		k = 0;

	for (int w = 0; w < nw; w++)
	{
		const uint64 bits = mask[w] & (w == nw - 1 ? AK6_TAIL(n) : ~UINT64CONST(0));

		for (int h = 0; h < 2; h++)
		{
			const __mmask32 m = (__mmask32) (bits >> (32 * h));

			if (m)
			{
				_mm512_mask_compressstoreu_epi16(sel + k, m,
												 _mm512_add_epi16(LD(iota), _mm512_set1_epi16((short) (w * 64 + 32 * h))));
				k += _mm_popcnt_u32(m);
			}
		}
	}
	return k;
}

/* dst &= kp, or dst &= (~kp & doc_valid), over n bits */
F5 uint16
key_present_apply_avx512(uint64 *dst, const uint64 *kp, const uint64 *doc_valid, uint16 n, bool negate)
{
	const int	nw = AK6_NW(n),
				full = nw - ((n & 63) != 0);
	uint32		pc = 0;
	int			w = 0;

	for (; w + 8 <= full; w += 8)
	{
		__m512i		x = _mm512_and_si512(LD(dst + w), negate ? _mm512_andnot_si512(LD(kp + w), LD(doc_valid + w)) : LD(kp + w));

		ST(dst + w, x);
		pc += (uint32) _mm512_reduce_add_epi64(_mm512_popcnt_epi64(x));
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

F5 void
hash_i64_avx512(uint64 *h, const int64 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 8 <= n; i += 8)
		ST(h + i, hash_step(mix ? LD(h + i) : _mm512_setzero_si512(), ld64x8(v, sel, i), mix));
	for (; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) v[ROW_OF_SEL(sel, i)], mix);
}

F5 void
hash_i32_avx512(uint64 *h, const int32 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 8 <= n; i += 8)
		ST(h + i, hash_step(mix ? LD(h + i) : _mm512_setzero_si512(), ld32x8(v, sel, i), mix));
	for (; i < n; i++)
		h[i] = AK6_HASH_STEP(h[i], (uint64) (int64) v[ROW_OF_SEL(sel, i)], mix);
}

F5 void
hash_i128_avx512(uint64 *h, const SiftI128 *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m512i		lo,
					hi;

		ld128x8(v, sel, i, &lo, &hi);
		ST(h + i, hash_step(hash_step(mix ? LD(h + i) : _mm512_setzero_si512(), lo, mix), hi, true));
	}
	for (; i < n; i++)
	{
		const SiftI128 *x = &v[ROW_OF_SEL(sel, i)];

		h[i] = AK6_HASH_STEP(AK6_HASH_STEP(h[i], x->lo, mix), (uint64) x->hi, true);
	}
}

/* A view of at most 12 bytes hashes by its two halves with no dereference; the longer views of a block of eight (up to two) are hashed by the scalar function, a block with more all of it */
F5 void
hash_strview_avx512(uint64 *h, const SiftStrView *v, const uint16 *sel, uint16 n, bool mix)
{
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m512i		hdr,
					tail;
		__mmask8	longs;

		ld128x8(v, sel, i, &hdr, &tail);
		longs = _mm512_cmpgt_epi64_mask(_mm512_and_si512(hdr, LO32), SET64(12));
		if (__builtin_popcount(longs) > 2)
		{
			for (int j = i; j < i + 8; j++)
				h[j] = ak6_sv_hash(h[j], &v[ROW_OF_SEL(sel, j)], mix);
			continue;
		}
		{
			__m512i		r = hash_step(hash_step(mix ? LD(h + i) : _mm512_setzero_si512(), hdr, mix), tail, true);

			if (longs)
			{
				uint64		lane[8];

				ST(lane, r);
				for (int j = 0; j < 8; j++)
					if ((longs >> j) & 1)
						lane[j] = ak6_sv_hash(h[i + j], &v[ROW_OF_SEL(sel, i + j)], mix);
				memcpy(h + i, lane, sizeof(lane));
			}
			else
				ST(h + i, r);
		}
	}
	for (; i < n; i++)
		h[i] = ak6_sv_hash(h[i], &v[ROW_OF_SEL(sel, i)], mix);
}

/* ---- reductions ---- */

F5 int64
sum_i32_avx512(const int32 *v, const uint64 *mask, uint16 n)
{
	__m512i		acc = _mm512_setzero_si512();
	int64		s;
	int			i = 0;

	for (; i + 16 <= n; i += 16)
	{
		__m512i		x = _mm512_maskz_loadu_epi32(mask ? (__mmask16) BITS_AT(mask, i, 0xFFFF) : (__mmask16) 0xFFFF, v + i);

		acc = _mm512_add_epi64(acc, _mm512_add_epi64(_mm512_cvtepi32_epi64(_mm512_castsi512_si256(x)),
													 _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(x, 1))));
	}
	s = _mm512_reduce_add_epi64(acc);
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
			s += v[i];
	return s;
}

F5 SiftI128
sum_i64_to_i128_avx512(const int64 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	__m512i		a = _mm512_setzero_si512(),
				b = a,
				c = a;
	ak6_u128	s = ak6_u128_of(init);
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m512i		x = _mm512_maskz_loadu_epi64(mask ? (__mmask8) BITS_AT(mask, i, 0xFF) : (__mmask8) 0xFF, v + i),
					u = _mm512_srli_epi64(x, 32);

		a = _mm512_add_epi64(a, _mm512_and_si512(x, LO32));
		b = _mm512_add_epi64(b, u);
		c = _mm512_add_epi64(c, _mm512_srli_epi64(u, 31));
	}
	s += ak6_split_total((uint64) _mm512_reduce_add_epi64(a), (uint64) _mm512_reduce_add_epi64(b),
						 (uint64) _mm512_reduce_add_epi64(c));
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
			s += (ak6_u128) (__int128) v[i];
	return ak6_i128_of(s);
}

F5 bool
sum_i64_checked_avx512(const int64 *v, const uint64 *mask, uint16 n, int64 *out)
{
	const SiftI128 zero = {0, 0};

	return ak6_fits_i64(sum_i64_to_i128_avx512(v, mask, n, zero), out);
}

/* Four 128-bit values per register: the mask register holds each row bit twice */
F5 SiftI128
sum_i128_avx512(const SiftI128 *v, const uint64 *mask, uint16 n, SiftI128 init)
{
	static const uint8 pair[16] = {0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F, 0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
	__m512i		a = _mm512_setzero_si512(),
				b = a;
	uint64		la[8],
				lb[8];
	ak6_u128	s = ak6_u128_of(init);
	uint64		hi = 0;
	int			i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m512i		x = _mm512_maskz_loadu_epi64(mask ? (__mmask8) pair[BITS_AT(mask, i, 15)] : (__mmask8) 0xFF, v + i);

		a = _mm512_add_epi64(a, _mm512_and_si512(x, LO32));
		b = _mm512_add_epi64(b, _mm512_srli_epi64(x, 32));
	}
	ST(la, a);
	ST(lb, b);
	for (int j = 0; j < 8; j += 2)
	{
		s += (ak6_u128) la[j] + ((ak6_u128) lb[j] << 32);
		hi += la[j + 1] + (lb[j + 1] << 32);
	}
	s += (ak6_u128) hi << 64;
	for (; i < n; i++)
		if (!mask || ROWBIT(mask, i))
			s += ak6_u128_of(v[i]);
	return ak6_i128_of(s);
}

F5 double
sum_f64_fast_avx512(const double *v, const uint64 *mask, uint16 n, double init)
{
	__m512d		acc = _mm512_setzero_pd();
	double		a[8];
	int			i = 0;

	for (; i + 8 <= n; i += 8)
		acc = _mm512_add_pd(acc, _mm512_maskz_loadu_pd(mask ? (__mmask8) BITS_AT(mask, i, 0xFF) : (__mmask8) 0xFF, v + i));
	_mm512_storeu_pd(a, acc);
	for (; i < n; i++)
		a[i & 7] += (!mask || ROWBIT(mask, i)) ? v[i] : 0.0;
	return ak6_f64_tree(a, init);
}

#define AK6_INIT5(s)	.s = s##_avx512,
#define AK6_INIT5X(s)	AK6_INIT_AT(s, avx512)

#define AK6_COMMON_AVX512 \
	AK6_INIT5(mask_andn_del) AK6_INIT5(mask_and) AK6_INIT5(mask_to_sel16) AK6_INIT5(key_present_apply) \
	AK6_INIT5(hash_i32) AK6_INIT5(hash_i64) AK6_INIT5(hash_i128) AK6_INIT5(hash_strview) \
	AK6_INIT5(sum_i32) AK6_INIT5(sum_i64_checked) AK6_INIT5(sum_i64_to_i128) AK6_INIT5(sum_i128) AK6_INIT5(sum_f64_fast) \
	AK6_OTHER_AVX512(AK6_INIT5X) \
	AK6_FMV_SLOTS(AK6_INIT_FMV) \
	AK6_ADDED_SLOTS(AK6_INIT5X)

const SiftAggKernelsV6 sift_aggkern6_avx512 = {
	AK6_COMMON_AVX512
	.count_gid = ak6_g_count_gid,
	.sum_gid_i32 = ak6_g_sum_gid_i32,
	.sum_gid_i64 = ak6_g_sum_gid_i64,
	.sum_gid_i64_to_i128 = ak6_g_sum_gid_i64_to_i128,
	.sum_gid_i128 = ak6_g_sum_gid_i128,
	.sum_gid_f64_fast = ak6_g_sum_gid_f64_fast,
	.minmax_gid_i64 = ak6_g_minmax_gid_i64,
	.minmax_gid_f64 = ak6_g_minmax_gid_f64,
};

const SiftAggKernelsV6 sift_aggkern6_avx512_conflict = {
	AK6_COMMON_AVX512
	AK6_CONFLICT_AVX512(AK6_INIT5X)
};

#endif							/* __x86_64__ */
