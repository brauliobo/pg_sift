/*
 * col/kern_avx2.c - AVX2 column kernels (docs/COL.md section 8); every slot is bit-identical to the scalar table.
 *
 * A range test is one subtraction and one signed compare: with the sign bit of the element width added to lo and xored into span,
 * (x - lo) <= span unsigned is !((x - lo') > span') signed. A 64-element word comes out of movemasks (u16 and u32 compare results
 * are packed to bytes first). The 8-bit LUT is a two-register PSHUFB lookup, wider LUTs use gathers; unpack cuts 8 values per step
 * from two 128-bit windows of the stream with PSHUFB and variable shifts. bits_count is the bitmap module's AVX2 popcount; bits_to_idx
 * walks a sparse word bit by bit and expands a denser one a byte at a time through a 256 x 8 position table. The selection decodes (for_i64_sel, gather_u32) stay scalar: gathers were slower than the loads they
 * replace (docs/COL.md section 8, Measured). The last partial word of a bit vector is computed from a zero padded copy of the
 * remaining elements.
 */
#include "core/host.h"

#include "col/kern_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K2 static inline __attribute__((always_inline)) SIFT_TARGET_AVX2
#define F2 static SIFT_TARGET_AVX2

SIFT_COLK_BITS_COUNT(avx2, sift_kern_avx2)

K2 __m256i
ld(const void *p)
{
	return _mm256_loadu_si256((const __m256i *) p);
}

K2 void
st(void *p, __m256i v)
{
	_mm256_storeu_si256((__m256i *) p, v);
}

/* The 64 results of the elements at p, bit k for element k; lo and span are the biased constants */
K2 uint64
range8w(const uint8 *p, __m256i lo, __m256i span)
{
	uint32		a = ~_mm256_movemask_epi8(_mm256_cmpgt_epi8(_mm256_sub_epi8(ld(p), lo), span)),
				b = ~_mm256_movemask_epi8(_mm256_cmpgt_epi8(_mm256_sub_epi8(ld(p + 32), lo), span));

	return (uint64) b << 32 | a;
}

K2 uint32
range16x32(const uint16 *p, __m256i lo, __m256i span)
{
	__m256i		c0 = _mm256_cmpgt_epi16(_mm256_sub_epi16(ld(p), lo), span),
				c1 = _mm256_cmpgt_epi16(_mm256_sub_epi16(ld(p + 16), lo), span);

	return ~_mm256_movemask_epi8(_mm256_permute4x64_epi64(_mm256_packs_epi16(c0, c1), 0xD8));
}

K2 uint64
range16w(const uint16 *p, __m256i lo, __m256i span)
{
	return (uint64) range16x32(p + 32, lo, span) << 32 | range16x32(p, lo, span);
}

K2 uint32
range32x32(const uint32 *p, __m256i lo, __m256i span)
{
	__m256i		c0 = _mm256_cmpgt_epi32(_mm256_sub_epi32(ld(p), lo), span),
				c1 = _mm256_cmpgt_epi32(_mm256_sub_epi32(ld(p + 8), lo), span),
				c2 = _mm256_cmpgt_epi32(_mm256_sub_epi32(ld(p + 16), lo), span),
				c3 = _mm256_cmpgt_epi32(_mm256_sub_epi32(ld(p + 24), lo), span);
	__m256i		b = _mm256_packs_epi16(_mm256_packs_epi32(c0, c1), _mm256_packs_epi32(c2, c3));

	return ~_mm256_movemask_epi8(_mm256_permutevar8x32_epi32(b, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7)));
}

K2 uint64
range32w(const uint32 *p, __m256i lo, __m256i span)
{
	return (uint64) range32x32(p + 32, lo, span) << 32 | range32x32(p, lo, span);
}

K2 uint64
range64w(const uint64 *p, __m256i lo, __m256i span)
{
	uint64		m = 0;

	SIFT_COLK_UNROLL(16)
	for (int k = 0; k < 16; k++)
		m |= (uint64) _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(_mm256_sub_epi64(ld(p + 4 * k), lo), span))) << (4 * k);
	return ~m;
}

K2 __m256i
set8(uint64 v)
{
	return _mm256_set1_epi8((char) v);
}

K2 __m256i
set16(uint64 v)
{
	return _mm256_set1_epi16((short) v);
}

K2 __m256i
set32(uint64 v)
{
	return _mm256_set1_epi32((int) v);
}

K2 __m256i
set64(uint64 v)
{
	return _mm256_set1_epi64x((long long) v);
}

#define SIGN(T)		((T) ((T) 1 << (sizeof(T) * 8 - 1)))

#define RANGE_AVX2(name, T, set, word) \
F2 void \
name(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits) \
{ \
	const __m256i l = set((T) (lo + SIGN(T))), s = set((T) (span ^ SIGN(T))); \
	SIFT_COLK_BITS_OF(T, a, n, bits, word, l, s); \
}

RANGE_AVX2(range_u8_avx2, uint8, set8, range8w)
RANGE_AVX2(range_u16_avx2, uint16, set16, range16w)
RANGE_AVX2(range_u32_avx2, uint32, set32, range32w)
RANGE_AVX2(range_u64_avx2, uint64, set64, range64w)

/* 8-bit LUT: byte x >> 3 of the 32-byte set (tlo / thi: its halves, broadcast), then bit x & 7 of it (pw: the powers of two) */
K2 uint32
lut8x32(const uint8 *p, __m256i tlo, __m256i thi, __m256i pw)
{
	__m256i		x = ld(p),
				idx = _mm256_and_si256(_mm256_srli_epi16(x, 3), _mm256_set1_epi8(0x0F)),
				b = _mm256_blendv_epi8(_mm256_shuffle_epi8(tlo, idx), _mm256_shuffle_epi8(thi, idx), x),
				m = _mm256_shuffle_epi8(pw, _mm256_and_si256(x, _mm256_set1_epi8(7)));

	return ~_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(b, m), _mm256_setzero_si256()));
}

K2 uint64
lut8w(const uint8 *p, __m256i tlo, __m256i thi, __m256i pw)
{
	return (uint64) lut8x32(p + 32, tlo, thi, pw) << 32 | lut8x32(p, tlo, thi, pw);
}

F2 void
lut_u8_avx2(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	const __m256i tlo = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *) lutbits)),
				thi = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *) (lutbits + 2))),
				pw = _mm256_set1_epi64x(0x8040201008040201);

	SIFT_COLK_BITS_OF(uint8, a, n, bits, lut8w, tlo, thi, pw);
}

/* LUT bits of 8 codes: the dword x >> 5 of the set by gather, its bit x & 31 moved to the sign bit */
K2 uint32
lutg8(__m256i x, const uint64 *lutbits)
{
	__m256i		w = _mm256_i32gather_epi32((const int *) lutbits, _mm256_srli_epi32(x, 5), 4),
				r = _mm256_sllv_epi32(w, _mm256_sub_epi32(_mm256_set1_epi32(31), _mm256_and_si256(x, _mm256_set1_epi32(31))));

	return _mm256_movemask_ps(_mm256_castsi256_ps(r));
}

K2 uint64
lut16w(const uint16 *p, const uint64 *lutbits)
{
	uint64		m = 0;

	SIFT_COLK_UNROLL(8)
	for (int k = 0; k < 8; k++)
		m |= (uint64) lutg8(_mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *) (p + 8 * k))), lutbits) << (8 * k);
	return m;
}

K2 uint64
lut32w(const uint32 *p, const uint64 *lutbits)
{
	uint64		m = 0;

	SIFT_COLK_UNROLL(8)
	for (int k = 0; k < 8; k++)
		m |= (uint64) lutg8(ld(p + 8 * k), lutbits) << (8 * k);
	return m;
}

F2 void
lut_u16_avx2(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	SIFT_COLK_BITS_OF(uint16, a, n, bits, lut16w, lutbits);
}

F2 void
lut_u32_avx2(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	SIFT_COLK_BITS_OF(uint32, a, n, bits, lut32w, lutbits);
}

/* dst = dst op src over nwords words, 4 per step */
#define BITS_OP_AVX2(name, vec, scalar) \
F2 void \
name(uint64 *dst, const uint64 *src, uint32 nwords) \
{ \
	uint32		i = 0; \
	for (; i + 4 <= nwords; i += 4) \
		st(dst + i, vec(ld(dst + i), ld(src + i))); \
	for (; i < nwords; i++) \
		dst[i] = scalar(dst[i], src[i]); \
}

#define OP_AND(d, s)		((d) & (s))
#define OP_OR(d, s)		((d) | (s))
#define OP_ANDNOT(d, s)	((d) & ~(s))
#define VANDNOT(d, s)	_mm256_andnot_si256(s, d)

BITS_OP_AVX2(bits_and_avx2, _mm256_and_si256, OP_AND)
BITS_OP_AVX2(bits_or_avx2, _mm256_or_si256, OP_OR)
BITS_OP_AVX2(bits_andnot_avx2, VANDNOT, OP_ANDNOT)

F2 uint64
bits_and_count_avx2(const uint64 *a, const uint64 *b, uint32 nwords)
{
	uint64		c0 = 0,
				c1 = 0,
				c2 = 0,
				c3 = 0;
	uint32		i = 0;

	for (; i + 4 <= nwords; i += 4)
	{
		c0 += _mm_popcnt_u64(a[i] & b[i]);
		c1 += _mm_popcnt_u64(a[i + 1] & b[i + 1]);
		c2 += _mm_popcnt_u64(a[i + 2] & b[i + 2]);
		c3 += _mm_popcnt_u64(a[i + 3] & b[i + 3]);
	}
	for (; i < nwords; i++)
		c0 += _mm_popcnt_u64(a[i] & b[i]);
	return c0 + c1 + c2 + c3;
}

F2 uint32
bits_to_idx_avx2(const uint64 *bits, uint32 nwords, uint32 base, uint32 *idx)
{
	uint32	   *o = idx;

	for (uint32 w = 0; w < nwords; w++, base += 64)
	{
		uint64		m = bits[w];

		if (_mm_popcnt_u64(m) <= SIFT_COLK_SEL_SPARSE_AVX2)
		{
			for (; m; m = _blsr_u64(m))
				*o++ = base + _tzcnt_u64(m);
			continue;
		}
		SIFT_COLK_UNROLL(8)
		for (uint32 j = 0; j < 8; j++)
		{
			const uint32 b = (m >> (8 * j)) & 0xFF;

			st(o, _mm256_add_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *) sift_colk_lut_pos[b])),
								   _mm256_set1_epi32(base + 8 * j)));
			o += _mm_popcnt_u32(b);
		}
	}
	return o - idx;
}

/* Units to drop: max < lo or min > hi, 4 per step */
K2 uint64
zone_range_w(const int64 *mn, const int64 *mx, __m256i lo, __m256i hi)
{
	uint64		m = 0;

	SIFT_COLK_UNROLL(16)
	for (int k = 0; k < 16; k++)
	{
		__m256i		drop = _mm256_or_si256(_mm256_cmpgt_epi64(lo, ld(mx + 4 * k)), _mm256_cmpgt_epi64(ld(mn + 4 * k), hi));

		m |= (uint64) _mm256_movemask_pd(_mm256_castsi256_pd(drop)) << (4 * k);
	}
	return ~m;
}

F2 void
zone_range_avx2(const int64 *mn, const int64 *mx, uint32 n, int64 lo, int64 hi, uint64 *keep)
{
	const __m256i l = set64(lo),
				h = set64(hi);
	uint32		i = 0;

	for (; i + 64 <= n; i += 64)
		keep[i / 64] = zone_range_w(mn + i, mx + i, l, h);
	if (i < n)
	{
		int64		tn[64],
					tx[64];
		uint32		r = n - i;

		memcpy(tn, mn + i, r * sizeof(int64));
		memcpy(tx, mx + i, r * sizeof(int64));
		memset(tn + r, 0, (64 - r) * sizeof(int64));
		memset(tx + r, 0, (64 - r) * sizeof(int64));
		keep[i / 64] = zone_range_w(tn, tx, l, h) & sift_colk_tail(n, i / 64);
	}
	sift_colk_zero_pad(keep, SIFT_COLK_WORDS(n), n);
}

F2 void
zone_set_avx2(const uint64 (*set)[4], uint32 n, const uint64 *probe, uint64 *keep)
{
	const __m256i pr = ld(probe);
	uint32		w = 0;

	for (; (w + 1) * 64 <= n; w++)
	{
		uint64		m = 0;

#pragma GCC unroll 16
		for (uint32 j = 0; j < 64; j++)
			m |= (uint64) !_mm256_testz_si256(ld(set[w * 64 + j]), pr) << j;
		keep[w] &= m;
	}
	if (w * 64 < n)
	{
		uint64		m = 0;

		for (uint32 j = 0; w * 64 + j < n; j++)
			m |= (uint64) !_mm256_testz_si256(ld(set[w * 64 + j]), pr) << j;
		keep[w] &= m | ~sift_colk_tail(n, w);
	}
}

F2 void
zone_notnull_avx2(const uint32 *nnull, const uint32 *nvals, uint32 n, uint64 *keep)
{
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
	{
		uint64		eq = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(ld(nnull + i), ld(nvals + i))));

		keep[i / 64] &= ~(eq << (i & 63));
	}
	for (; i < n; i++)
		keep[i / 64] &= ~((uint64) (nnull[i] == nvals[i]) << (i & 63));
}

/*
 * 8 values of `bits` bits from the stream at g (byte aligned, 8 * bits bits): lane 0 holds the window of values 0..3, lane 1 that of
 * values 4..7 (starting lane1 bytes on). A value is the 32 bits at its byte (ca: bytes o..o+3), shifted by its bit (vs); for more
 * than 25 bits its top bits are in the next byte (cb: bytes o+1..o+4, shifted up by 32 - vs).
 */
K2 __m256i
unpack8(const uint8 *g, uint32 lane1, __m256i ca, __m256i cb, __m256i vs, __m256i vs2, __m256i mask, const bool wide)
{
	__m256i		w = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i *) g)),
											_mm_loadu_si128((const __m128i *) (g + lane1)), 1),
				v = _mm256_srlv_epi32(_mm256_shuffle_epi8(w, ca), vs);

	if (wide)
		v = _mm256_or_si256(v, _mm256_sllv_epi32(_mm256_srli_epi32(_mm256_shuffle_epi8(w, cb), 24), vs2));
	return _mm256_and_si256(v, mask);
}

K2 void
unpack_tmpl(const uint8 *src, uint32 bits, uint32 n, uint32 *dst, const bool wide)
{
	uint8		ca[32],
				cb[32];
	uint32		sh[8],
				sh2[8],
				lane1 = (4 * bits) >> 3,
				i = 0;

	for (uint32 j = 0; j < 8; j++)
	{
		uint32		rel = j * bits - (j >= 4 ? 8 * lane1 : 0);

		sh[j] = rel & 7;
		sh2[j] = 32 - (rel & 7);
		for (uint32 t = 0; t < 4; t++)
		{
			ca[(j >> 2) * 16 + (j & 3) * 4 + t] = (rel >> 3) + t;
			cb[(j >> 2) * 16 + (j & 3) * 4 + t] = (rel >> 3) + 1 + t;
		}
	}
	{
		const __m256i va = ld(ca),
					vb = ld(cb),
					vs = ld(sh),
					vs2 = ld(sh2),
					mask = _mm256_set1_epi32((1U << bits) - 1);

		for (; i + 8 <= n; i += 8)
			st(dst + i, unpack8(src + (uint64) (i >> 3) * bits, lane1, va, vb, vs, vs2, mask, wide));
		if (i < n)
		{
			uint32		tmp[8];

			st(tmp, unpack8(src + (uint64) (i >> 3) * bits, lane1, va, vb, vs, vs2, mask, wide));
			memcpy(dst + i, tmp, (n - i) * sizeof(uint32));
		}
	}
}

F2 void
unpack_avx2(const uint8 *src, uint32 bits, uint32 n, uint32 *dst)
{
	if (bits > 25)
		unpack_tmpl(src, bits, n, dst, true);
	else
		unpack_tmpl(src, bits, n, dst, false);
}

F2 void
for_i64_u8_avx2(const void *a, uint32 n, int64 base, int64 *dst)
{
	const uint8 *p = a;
	const __m256i b = set64(base);
	uint32		i = 0;

	for (; i + 16 <= n; i += 16)
	{
		__m128i		v = _mm_loadu_si128((const __m128i *) (p + i));

		st(dst + i, _mm256_add_epi64(_mm256_cvtepu8_epi64(v), b));
		st(dst + i + 4, _mm256_add_epi64(_mm256_cvtepu8_epi64(_mm_srli_si128(v, 4)), b));
		st(dst + i + 8, _mm256_add_epi64(_mm256_cvtepu8_epi64(_mm_srli_si128(v, 8)), b));
		st(dst + i + 12, _mm256_add_epi64(_mm256_cvtepu8_epi64(_mm_srli_si128(v, 12)), b));
	}
	sift_colk_for_i64_u8_scalar(p + i, n - i, base, dst + i);
}

F2 void
for_i64_u16_avx2(const void *a, uint32 n, int64 base, int64 *dst)
{
	const uint16 *p = a;
	const __m256i b = set64(base);
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m128i		v = _mm_loadu_si128((const __m128i *) (p + i));

		st(dst + i, _mm256_add_epi64(_mm256_cvtepu16_epi64(v), b));
		st(dst + i + 4, _mm256_add_epi64(_mm256_cvtepu16_epi64(_mm_srli_si128(v, 8)), b));
	}
	sift_colk_for_i64_u16_scalar(p + i, n - i, base, dst + i);
}

F2 void
for_i64_u32_avx2(const void *a, uint32 n, int64 base, int64 *dst)
{
	const uint32 *p = a;
	const __m256i b = set64(base);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
		st(dst + i, _mm256_add_epi64(_mm256_cvtepu32_epi64(_mm_loadu_si128((const __m128i *) (p + i))), b));
	sift_colk_for_i64_u32_scalar(p + i, n - i, base, dst + i);
}

F2 void
for_i64_u64_avx2(const void *a, uint32 n, int64 base, int64 *dst)
{
	const uint64 *p = a;
	const __m256i b = set64(base);
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
		st(dst + i, _mm256_add_epi64(ld(p + i), b));
	sift_colk_for_i64_u64_scalar(p + i, n - i, base, dst + i);
}

/* key ^ (the low 63 bits when the key is negative): the IEEE bits of the double a float key stands for */
F2 void
f8_from_keys_avx2(int64 *a, uint32 n)
{
	const __m256i zero = _mm256_setzero_si256();
	uint32		i = 0;

	for (; i + 4 <= n; i += 4)
	{
		__m256i		k = ld(a + i);

		st(a + i, _mm256_xor_si256(k, _mm256_srli_epi64(_mm256_cmpgt_epi64(zero, k), 1)));
	}
	sift_colk_f8_from_keys_scalar(a + i, n - i);
}

/* 32 flags a step: the four bytes of the mask each replicated over eight lanes, one bit kept per lane */
F2 void
bits_to_flags_avx2(const uint64 *bits, uint32 n, uint8 *flags)
{
	const __m256i pick = _mm256_set_epi64x(UINT64CONST(0x0303030303030303), UINT64CONST(0x0202020202020202),
										   UINT64CONST(0x0101010101010101), 0),
				  bit = _mm256_set1_epi64x(UINT64CONST(0x8040201008040201)),
				  one = _mm256_set1_epi8(1);

	for (uint32 i = 0; i < n; i += 32)
	{
		uint32		m = (uint32) (bits[i / 64] >> (i % 64));
		__m256i		x = _mm256_and_si256(_mm256_shuffle_epi8(_mm256_set1_epi32((int) m), pick), bit);

		st(flags + i, _mm256_and_si256(_mm256_cmpeq_epi8(x, bit), one));
	}
}

/* A parent's run is stored a register at a time, past its end too (the next run, or the padding, takes the tail): one store for a run of up to 8 */
F2 void
expand_parents_avx2(const uint32 *end, uint32 np, uint32 *parent)
{
	uint32		i = 0;

	for (uint32 p = 0; p < np; p++)
	{
		__m256i		v = _mm256_set1_epi32((int) p);

		for (; i < end[p]; i += 8)
			st(parent + i, v);
		i = end[p];
	}
}

const SiftColKernels sift_col_kernels_avx2 = {
	.range = {range_u8_avx2, range_u16_avx2, range_u32_avx2, range_u64_avx2},
	.lut = {lut_u8_avx2, lut_u16_avx2, lut_u32_avx2},
	.bits_and = bits_and_avx2,
	.bits_or = bits_or_avx2,
	.bits_andnot = bits_andnot_avx2,
	.bits_count = bits_count_avx2,
	.bits_and_count = bits_and_count_avx2,
	.bits_to_idx = bits_to_idx_avx2,
	.zone_range = zone_range_avx2,
	.zone_set = zone_set_avx2,
	.zone_notnull = zone_notnull_avx2,
	.unpack = unpack_avx2,
	.for_i64 = {for_i64_u8_avx2, for_i64_u16_avx2, for_i64_u32_avx2, for_i64_u64_avx2},
	.for_i64_sel = {sift_colk_for_sel_u8_scalar, sift_colk_for_sel_u16_scalar, sift_colk_for_sel_u32_scalar, sift_colk_for_sel_u64_scalar},
	.gather_u32 = sift_colk_gather_u32_scalar,
	.f8_from_keys = f8_from_keys_avx2,
	.bits_to_flags = bits_to_flags_avx2,
	.expand_parents = expand_parents_avx2,
};

#endif
