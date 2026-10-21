/*
 * col/kern_avx512.c - AVX-512 (F/BW/VL/DQ/VBMI2/VPOPCNTDQ) column kernels (docs/COL.md section 8); every slot is bit-identical
 * to the scalar table.
 *
 * Compares write mask registers, so a range test is a subtraction and one unsigned compare whose result is the bit vector word; the
 * tail of every array is a masked load, the padding is not read. The 8-bit LUT is a two-register PSHUFB lookup, wider LUTs use
 * gathers, bits_to_idx compresses the bit positions of a word with VPCOMPRESSB, zone_set tests two units per register. unpack cuts 16
 * values per step from four 128-bit windows of the stream. bits_count is the bitmap module's AVX-512 popcount. gather_u32 is a masked
 * VPGATHERDD; the other selection decodes (for_i64_sel) stay scalar: gathers were slower than the loads they replace (docs/COL.md
 * section 8, Measured).
 */
#include "sift_host.h"

#include "col/kern_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K5 static inline __attribute__((always_inline)) SIFT_TARGET_AVX512
#define F5 static SIFT_TARGET_AVX512

SIFT_COLK_BITS_COUNT(avx512, sift_kern_avx512)

/* The lanes of a vector of L that n - i elements fill: all of them, or the first n - i */
#define LANES(n, i, L)		_bzhi_u64(~UINT64CONST(0), Min((n) - (i), (L)))

/*
 * Fills the bit vector `bits` of n bits from vec(i, k, ...), the L result bits of the elements i.. of the lanes k (all ones for a
 * full vector; the bits of the lanes not in k must be zero), then zeroes its padding
 */
#define BITS512(L, n, bits, vec, ...) \
	do { \
		uint32		i_ = 0; \
		for (; i_ + 64 <= (n); i_ += 64) \
		{ \
			uint64		m_ = 0; \
			SIFT_COLK_UNROLL(64) \
			for (uint32 j_ = 0; j_ < 64; j_ += (L)) \
				m_ |= (uint64) vec(i_ + j_, ~UINT64CONST(0), __VA_ARGS__) << j_; \
			(bits)[i_ / 64] = m_; \
		} \
		if (i_ < (n)) \
		{ \
			uint64		m_ = 0; \
			for (uint32 j_ = 0; i_ + j_ < (n); j_ += (L)) \
				m_ |= (uint64) vec(i_ + j_, LANES(n, i_ + j_, L), __VA_ARGS__) << j_; \
			(bits)[i_ / 64] = m_; \
		} \
		sift_colk_zero_pad(bits, SIFT_COLK_WORDS(n), n); \
	} while (0)

K5 uint64
range8v(uint32 i, uint64 k, const uint8 *p, __m512i lo, __m512i span)
{
	return _mm512_mask_cmple_epu8_mask(k, _mm512_sub_epi8(_mm512_maskz_loadu_epi8(k, p + i), lo), span);
}

K5 uint64
range16v(uint32 i, uint64 k, const uint16 *p, __m512i lo, __m512i span)
{
	return _mm512_mask_cmple_epu16_mask(k, _mm512_sub_epi16(_mm512_maskz_loadu_epi16(k, p + i), lo), span);
}

K5 uint64
range32v(uint32 i, uint64 k, const uint32 *p, __m512i lo, __m512i span)
{
	return _mm512_mask_cmple_epu32_mask(k, _mm512_sub_epi32(_mm512_maskz_loadu_epi32(k, p + i), lo), span);
}

K5 uint64
range64v(uint32 i, uint64 k, const uint64 *p, __m512i lo, __m512i span)
{
	return _mm512_mask_cmple_epu64_mask(k, _mm512_sub_epi64(_mm512_maskz_loadu_epi64(k, p + i), lo), span);
}

F5 void
range_u8_avx512(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	BITS512(64, n, bits, range8v, a, _mm512_set1_epi8((char) lo), _mm512_set1_epi8((char) span));
}

F5 void
range_u16_avx512(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	BITS512(32, n, bits, range16v, a, _mm512_set1_epi16((short) lo), _mm512_set1_epi16((short) span));
}

F5 void
range_u32_avx512(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	BITS512(16, n, bits, range32v, a, _mm512_set1_epi32((int) lo), _mm512_set1_epi32((int) span));
}

F5 void
range_u64_avx512(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits)
{
	BITS512(8, n, bits, range64v, a, _mm512_set1_epi64((long long) lo), _mm512_set1_epi64((long long) span));
}

/* 8-bit LUT: byte x >> 3 of the 32-byte set (tlo / thi: its halves, repeated per lane), then bit x & 7 of it (pw: the powers of two) */
K5 uint64
lut8v(uint32 i, uint64 k, const uint8 *p, __m512i tlo, __m512i thi, __m512i pw)
{
	__m512i		x = _mm512_maskz_loadu_epi8(k, p + i),
				idx = _mm512_and_si512(_mm512_srli_epi16(x, 3), _mm512_set1_epi8(0x0F)),
				b = _mm512_mask_blend_epi8(_mm512_movepi8_mask(x), _mm512_shuffle_epi8(tlo, idx), _mm512_shuffle_epi8(thi, idx)),
				m = _mm512_shuffle_epi8(pw, _mm512_and_si512(x, _mm512_set1_epi8(7)));

	return _mm512_mask_test_epi8_mask(k, b, m);
}

F5 void
lut_u8_avx512(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	const __m512i tlo = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *) lutbits)),
				thi = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *) (lutbits + 2))),
				pw = _mm512_set1_epi64(0x8040201008040201);

	BITS512(64, n, bits, lut8v, a, tlo, thi, pw);
}

/* LUT bits of 16 codes: the dword x >> 5 of the set by gather, tested against bit x & 31 (rotate counts are taken mod 32) */
K5 uint64
lutg16(__m512i x, uint64 k, const uint64 *lutbits)
{
	__m512i		w = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), k, _mm512_srli_epi32(x, 5), lutbits, 4);

	return _mm512_mask_test_epi32_mask(k, w, _mm512_rolv_epi32(_mm512_set1_epi32(1), x));
}

K5 uint64
lut16v(uint32 i, uint64 k, const uint16 *p, const uint64 *lutbits)
{
	return lutg16(_mm512_cvtepu16_epi32(_mm256_maskz_loadu_epi16(k, p + i)), k, lutbits);
}

K5 uint64
lut32v(uint32 i, uint64 k, const uint32 *p, const uint64 *lutbits)
{
	return lutg16(_mm512_maskz_loadu_epi32(k, p + i), k, lutbits);
}

F5 void
lut_u16_avx512(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	BITS512(16, n, bits, lut16v, a, lutbits);
}

F5 void
lut_u32_avx512(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits)
{
	BITS512(16, n, bits, lut32v, a, lutbits);
}

/* dst = dst op src over nwords words, 8 per step, the last step masked */
#define BITS_OP_AVX512(name, vec) \
F5 void \
name(uint64 *dst, const uint64 *src, uint32 nwords) \
{ \
	uint32		i = 0; \
	for (; i + 8 <= nwords; i += 8) \
		_mm512_storeu_si512(dst + i, vec(_mm512_loadu_si512(dst + i), _mm512_loadu_si512(src + i))); \
	if (i < nwords) \
	{ \
		__mmask8	k = LANES(nwords, i, 8); \
		_mm512_mask_storeu_epi64(dst + i, k, vec(_mm512_maskz_loadu_epi64(k, dst + i), _mm512_maskz_loadu_epi64(k, src + i))); \
	} \
}

#define VANDNOT(d, s)	_mm512_andnot_si512(s, d)

BITS_OP_AVX512(bits_and_avx512, _mm512_and_si512)
BITS_OP_AVX512(bits_or_avx512, _mm512_or_si512)
BITS_OP_AVX512(bits_andnot_avx512, VANDNOT)

F5 uint64
bits_and_count_avx512(const uint64 *a, const uint64 *b, uint32 nwords)
{
	__m512i		acc = _mm512_setzero_si512();
	uint32		i = 0;

	for (; i + 8 <= nwords; i += 8)
		acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(_mm512_and_si512(_mm512_loadu_si512(a + i), _mm512_loadu_si512(b + i))));
	if (i < nwords)
	{
		__mmask8	k = LANES(nwords, i, 8);

		acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(_mm512_and_si512(_mm512_maskz_loadu_epi64(k, a + i), _mm512_maskz_loadu_epi64(k, b + i))));
	}
	return _mm512_reduce_add_epi64(acc);
}

/* Stores the first c (<= 16) of the byte positions in p, as dwords plus base, at o */
K5 void
store_pos(uint32 *o, __m128i p, __m512i base, uint32 c)
{
	_mm512_mask_storeu_epi32(o, _bzhi_u32(0xFFFF, c), _mm512_add_epi32(_mm512_cvtepu8_epi32(p), base));
}

F5 uint32
bits_to_idx_avx512(const uint64 *bits, uint32 nwords, uint32 base, uint32 *idx)
{
	const __m512i iota = _mm512_set_epi64(0x3F3E3D3C3B3A3938, 0x3736353433323130, 0x2F2E2D2C2B2A2928, 0x2726252423222120,
										  0x1F1E1D1C1B1A1918, 0x1716151413121110, 0x0F0E0D0C0B0A0908, 0x0706050403020100);
	uint32	   *o = idx;

	for (uint32 w = 0; w < nwords; w++, base += 64)
	{
		uint64		m = bits[w];
		uint32		c = _mm_popcnt_u64(m);
		__m512i		pos,
					vb;

		if (c <= SIFT_COLK_SEL_SPARSE_AVX512)
		{
			for (; m; m = _blsr_u64(m))
				*o++ = base + _tzcnt_u64(m);
			continue;
		}
		pos = _mm512_maskz_compress_epi8(m, iota);
		vb = _mm512_set1_epi32(base);
		store_pos(o, _mm512_castsi512_si128(pos), vb, c);
		if (c > 16)
			store_pos(o + 16, _mm512_extracti32x4_epi32(pos, 1), vb, c - 16);
		if (c > 32)
			store_pos(o + 32, _mm512_extracti32x4_epi32(pos, 2), vb, c - 32);
		if (c > 48)
			store_pos(o + 48, _mm512_extracti32x4_epi32(pos, 3), vb, c - 48);
		o += c;
	}
	return o - idx;
}

/* Units to keep: lo <= max and min <= hi, 8 per step */
K5 uint64
zone_range_v(uint32 i, uint64 k, const int64 *mn, const int64 *mx, __m512i lo, __m512i hi)
{
	return _mm512_mask_cmple_epi64_mask(_mm512_mask_cmple_epi64_mask(k, lo, _mm512_maskz_loadu_epi64(k, mx + i)),
										_mm512_maskz_loadu_epi64(k, mn + i), hi);
}

F5 void
zone_range_avx512(const int64 *mn, const int64 *mx, uint32 n, int64 lo, int64 hi, uint64 *keep)
{
	BITS512(8, n, keep, zone_range_v, mn, mx, _mm512_set1_epi64(lo), _mm512_set1_epi64(hi));
}

/* The 16 keep bits of units i..: bit u is set when set[u] & probe has a bit; 8 registers of two units each, masked past n unless full */
K5 uint64
zone_set_v(const uint64 (*set)[4], uint32 i, uint32 n, __m512i probe, const bool full)
{
	uint64		r = 0;

	for (uint32 b = 0; b < 8; b++)
	{
		int			rem = (int) n - (int) (i + 2 * b);
		__mmask8	k = full ? 0xFF : rem <= 0 ? 0 : _bzhi_u32(0xFF, Min(rem, 2) * 4);

		r |= (uint64) _mm512_test_epi64_mask(_mm512_maskz_loadu_epi64(k, set + i + 2 * b), probe) << (8 * b);
	}
	r |= r >> 1;
	r |= r >> 2;
	return _pext_u64(r, UINT64CONST(0x1111111111111111));
}

F5 void
zone_set_avx512(const uint64 (*set)[4], uint32 n, const uint64 *probe, uint64 *keep)
{
	const __m512i pr = _mm512_broadcast_i64x4(_mm256_loadu_si256((const __m256i *) probe));

	for (uint32 w = 0; w < SIFT_COLK_WORDS(n); w++)
	{
		uint64		m = 0;

		for (uint32 g = 0, u = w * 64; g < 4 && u < n; g++, u += 16)
			m |= (u + 16 <= n ? zone_set_v(set, u, n, pr, true) : zone_set_v(set, u, n, pr, false)) << (16 * g);
		keep[w] &= m | ~sift_colk_tail(n, w);
	}
}

F5 void
zone_notnull_avx512(const uint32 *nnull, const uint32 *nvals, uint32 n, uint64 *keep)
{
	uint32		i = 0;

	for (; i + 16 <= n; i += 16)
		keep[i / 64] &= ~((uint64) _mm512_cmpeq_epi32_mask(_mm512_loadu_si512(nnull + i), _mm512_loadu_si512(nvals + i)) << (i & 63));
	if (i < n)
	{
		__mmask16	k = LANES(n, i, 16);

		keep[i / 64] &= ~((uint64) _mm512_mask_cmpeq_epi32_mask(k, _mm512_maskz_loadu_epi32(k, nnull + i), _mm512_maskz_loadu_epi32(k, nvals + i)) << (i & 63));
	}
}

/*
 * 16 values of `bits` bits from the stream at g (byte aligned, 16 * bits bits): lane l holds the window of values 4l..4l+3, which
 * starts lb[l] bytes on. A value is the 32 bits at its byte (ca: bytes o..o+3), shifted by its bit (vs); for more than 25 bits its
 * top bits are in the next byte (cb: bytes o+1..o+4, shifted up by 32 - vs).
 */
K5 __m512i
unpack16(const uint8 *g, const uint32 *lb, __m512i ca, __m512i cb, __m512i vs, __m512i vs2, __m512i mask, const bool wide)
{
	__m512i		w = _mm512_castsi128_si512(_mm_loadu_si128((const __m128i *) (g + lb[0]))),
				v;

	w = _mm512_inserti32x4(w, _mm_loadu_si128((const __m128i *) (g + lb[1])), 1);
	w = _mm512_inserti32x4(w, _mm_loadu_si128((const __m128i *) (g + lb[2])), 2);
	w = _mm512_inserti32x4(w, _mm_loadu_si128((const __m128i *) (g + lb[3])), 3);
	v = _mm512_srlv_epi32(_mm512_shuffle_epi8(w, ca), vs);
	if (wide)
		v = _mm512_or_si512(v, _mm512_sllv_epi32(_mm512_srli_epi32(_mm512_shuffle_epi8(w, cb), 24), vs2));
	return _mm512_and_si512(v, mask);
}

K5 void
unpack_tmpl(const uint8 *src, uint32 bits, uint32 n, uint32 *dst, const bool wide)
{
	uint8		ca[64],
				cb[64];
	uint32		sh[16],
				sh2[16],
				lb[4],
				i = 0;

	for (uint32 l = 0; l < 4; l++)
		lb[l] = (4 * l * bits) >> 3;
	for (uint32 j = 0; j < 16; j++)
	{
		uint32		rel = (4 * (j >> 2) * bits & 7) + (j & 3) * bits;

		sh[j] = rel & 7;
		sh2[j] = 32 - (rel & 7);
		for (uint32 t = 0; t < 4; t++)
		{
			ca[(j >> 2) * 16 + (j & 3) * 4 + t] = (rel >> 3) + t;
			cb[(j >> 2) * 16 + (j & 3) * 4 + t] = (rel >> 3) + 1 + t;
		}
	}
	{
		const __m512i va = _mm512_loadu_si512(ca),
					vb = _mm512_loadu_si512(cb),
					vs = _mm512_loadu_si512(sh),
					vs2 = _mm512_loadu_si512(sh2),
					mask = _mm512_set1_epi32((1U << bits) - 1);

		for (; i + 16 <= n; i += 16)
			_mm512_storeu_si512(dst + i, unpack16(src + (uint64) (i >> 4) * 2 * bits, lb, va, vb, vs, vs2, mask, wide));
		if (i < n)
			_mm512_mask_storeu_epi32(dst + i, LANES(n, i, 16), unpack16(src + (uint64) (i >> 4) * 2 * bits, lb, va, vb, vs, vs2, mask, wide));
	}
}

F5 void
unpack_avx512(const uint8 *src, uint32 bits, uint32 n, uint32 *dst)
{
	if (bits > 25)
		unpack_tmpl(src, bits, n, dst, true);
	else
		unpack_tmpl(src, bits, n, dst, false);
}

F5 void
gather_u32_avx512(const uint32 *a, const uint32 *idx, uint32 n, uint32 *dst)
{
	uint32		i = 0;

	for (; i + 16 <= n; i += 16)
		_mm512_storeu_si512(dst + i, _mm512_i32gather_epi32(_mm512_loadu_si512(idx + i), a, 4));
	if (i < n)
	{
		__mmask16	k = LANES(n, i, 16);

		_mm512_mask_storeu_epi32(dst + i, k, _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), k, _mm512_maskz_loadu_epi32(k, idx + i), a, 4));
	}
}

/* dst[i..] = base + zero extended elements of 8 lanes: load(k, i) fetches the elements in the lanes k as qwords */
#define FOR_I64_AVX512(name, T, load) \
F5 void \
name(const void *a, uint32 n, int64 base, int64 *dst) \
{ \
	const T    *p = a; \
	const __m512i b = _mm512_set1_epi64(base); \
	uint32		i = 0; \
	for (; i + 8 <= n; i += 8) \
		_mm512_storeu_si512(dst + i, _mm512_add_epi64(load(0xFF, p + i), b)); \
	if (i < n) \
	{ \
		__mmask8	k = LANES(n, i, 8); \
		_mm512_mask_storeu_epi64(dst + i, k, _mm512_add_epi64(load(k, p + i), b)); \
	} \
}

#define LD8(k, p)	_mm512_cvtepu8_epi64(_mm_maskz_loadu_epi8(k, p))
#define LD16(k, p)	_mm512_cvtepu16_epi64(_mm_maskz_loadu_epi16(k, p))
#define LD32(k, p)	_mm512_cvtepu32_epi64(_mm256_maskz_loadu_epi32(k, p))
#define LD64(k, p)	_mm512_maskz_loadu_epi64(k, p)

FOR_I64_AVX512(for_i64_u8_avx512, uint8, LD8)
FOR_I64_AVX512(for_i64_u16_avx512, uint16, LD16)
FOR_I64_AVX512(for_i64_u32_avx512, uint32, LD32)
FOR_I64_AVX512(for_i64_u64_avx512, uint64, LD64)

F5 void
f8_from_keys_avx512(int64 *a, uint32 n)
{
	for (uint32 i = 0; i < n; i += 8)
	{
		__mmask8	k = LANES(n, i, 8);
		__m512i		v = _mm512_maskz_loadu_epi64(k, a + i);

		_mm512_mask_storeu_epi64(a + i, k, _mm512_xor_si512(v, _mm512_srli_epi64(_mm512_srai_epi64(v, 63), 1)));
	}
}

/* 64 flags a step: the mask word is the byte mask of the stores */
F5 void
bits_to_flags_avx512(const uint64 *bits, uint32 n, uint8 *flags)
{
	const __m512i one = _mm512_set1_epi8(1);

	for (uint32 i = 0; i < n; i += 64)
		_mm512_storeu_si512(flags + i, _mm512_maskz_mov_epi8(bits[i / 64], one));
}

/* A parent's run is stored 16 elements at a time, past its end too (the next run, or the padding, takes the tail): one store for a run of up to 16 */
F5 void
expand_parents_avx512(const uint32 *end, uint32 np, uint32 *parent)
{
	uint32		i = 0;

	for (uint32 p = 0; p < np; p++)
	{
		__m512i		v = _mm512_set1_epi32((int) p);

		for (; i < end[p]; i += 16)
			_mm512_storeu_si512(parent + i, v);
		i = end[p];
	}
}

const SiftColKernels sift_col_kernels_avx512 = {
	.range = {range_u8_avx512, range_u16_avx512, range_u32_avx512, range_u64_avx512},
	.lut = {lut_u8_avx512, lut_u16_avx512, lut_u32_avx512},
	.bits_and = bits_and_avx512,
	.bits_or = bits_or_avx512,
	.bits_andnot = bits_andnot_avx512,
	.bits_count = bits_count_avx512,
	.bits_and_count = bits_and_count_avx512,
	.bits_to_idx = bits_to_idx_avx512,
	.zone_range = zone_range_avx512,
	.zone_set = zone_set_avx512,
	.zone_notnull = zone_notnull_avx512,
	.unpack = unpack_avx512,
	.for_i64 = {for_i64_u8_avx512, for_i64_u16_avx512, for_i64_u32_avx512, for_i64_u64_avx512},
	.for_i64_sel = {sift_colk_for_sel_u8_scalar, sift_colk_for_sel_u16_scalar, sift_colk_for_sel_u32_scalar, sift_colk_for_sel_u64_scalar},
	.gather_u32 = gather_u32_avx512,
	.f8_from_keys = f8_from_keys_avx512,
	.bits_to_flags = bits_to_flags_avx512,
	.expand_parents = expand_parents_avx512,
};

#endif
