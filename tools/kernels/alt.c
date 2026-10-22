/*
 * Rejected bm25_accum designs, kept for the benchmark (same results bit for
 * bit; the bench checks it before timing):
 *   alt_bm25_ymm      AVX-512VL with 8-lane ymm chunks instead of 16-lane zmm
 *   alt_bm25_compress compress knorm of set slots into a dense array, score
 *                     full vectors, expand the products back (fewer divisions
 *                     on sparse masks, three passes)
 */
#include "harness.h"

#include <immintrin.h>

#define IN_REG(x) __asm__("" : "+v"(x))

int			alt_bm25_ymm(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
						 const float4 *knorm, float4 w);
int			alt_bm25_compress(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
							  const float4 *knorm, float4 w, float4 *dense);

SIFT_TARGET_AVX512 int
alt_bm25_ymm(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
			 const float4 *knorm, float4 w)
{
	const __m256 vw = _mm256_set1_ps(w);
	int			r = 0;

	for (int i = 0; i < SIFT_KERN_WORDS(npages); i++)
		for (uint64 m = mask[i]; m;)
		{
			int			c = __builtin_ctzll(m) & ~7,
						s = i * 64 + c;
			__mmask8	k = (__mmask8) (m >> c);
			__m128i		b = _mm_loadu_si128((const __m128i *) (tf + r));
			__m256		t,
						x;

			IN_REG(b);
			t = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_maskz_expand_epi8(k, b)));
			x = _mm256_maskz_div_ps(k, t, _mm256_maskz_add_ps(k, t, _mm256_load_ps(knorm + s)));

			_mm256_mask_store_ps(scores + s, k, _mm256_maskz_add_ps(k, _mm256_load_ps(scores + s), _mm256_mul_ps(vw, x)));
			r += __builtin_popcount(k);
			m &= ~(UINT64CONST(0xFF) << c);
		}
	return r;
}

SIFT_TARGET_AVX512 int
alt_bm25_compress(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
				  const float4 *knorm, float4 w, float4 *dense)
{
	const __m512 vw = _mm512_set1_ps(w);
	int			n = 0,
				j = 0;

	for (int i = 0; i < SIFT_KERN_WORDS(npages); i++)
		for (int c = 0; c < 64; c += 16)
		{
			__mmask16	k = (__mmask16) (mask[i] >> c);

			_mm512_storeu_ps(dense + n, _mm512_maskz_compress_ps(k, _mm512_load_ps(knorm + i * 64 + c)));
			n += __builtin_popcount(k);
		}
	for (int i = 0; i < n; i += 16)
	{
		__mmask16	k = n - i >= 16 ? 0xFFFF : (__mmask16) ((1u << (n - i)) - 1);
		__m512		t = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *) (tf + i))));
		__m512		x = _mm512_maskz_div_ps(k, t, _mm512_maskz_add_ps(k, t, _mm512_loadu_ps(dense + i)));

		_mm512_storeu_ps(dense + i, _mm512_mul_ps(vw, x));
	}
	for (int i = 0; i < SIFT_KERN_WORDS(npages); i++)
		for (uint64 m = mask[i]; m;)
		{
			int			c = __builtin_ctzll(m) & ~15,
						s = i * 64 + c;
			__mmask16	k = (__mmask16) (m >> c);
			__m512		v = _mm512_loadu_ps(dense + j);

			IN_REG(v);
			v = _mm512_maskz_expand_ps(k, v);

			_mm512_mask_store_ps(scores + s, k, _mm512_maskz_add_ps(k, _mm512_load_ps(scores + s), v));
			j += __builtin_popcount(k);
			m &= ~(UINT64CONST(0xFFFF) << c);
		}
	return n;
}

/* tf placement variants for the AVX-512 kernel (same loop as release) */
#define BELOW(m, j)	__builtin_popcount((m) & ((1 << (j)) - 1))
#define XL(m, j)	((((m) >> (j)) & 1) ? BELOW(m, j) : 0x80)
#define XROW(m)		{XL(m, 0), XL(m, 1), XL(m, 2), XL(m, 3), XL(m, 4), XL(m, 5), XL(m, 6), XL(m, 7)}
#define R4(m)		XROW(m), XROW(m + 1), XROW(m + 2), XROW(m + 3)
#define R16(m)		R4(m), R4(m + 4), R4(m + 8), R4(m + 12)
#define R64(m)		R16(m), R16(m + 16), R16(m + 32), R16(m + 48)
static const uint8 lut8[256][8] = {R64(0), R64(64), R64(128), R64(192)};

int			alt_bm25_tfvar(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
						   const float4 *knorm, float4 w, int var);

static inline __attribute__((always_inline)) SIFT_TARGET_AVX512 __m512
tf_lanes(const uint8 *tf, __mmask16 k, const int var)
{
	__m128i		b = _mm_loadu_si128((const __m128i *) tf);

	IN_REG(b);
	if (var == 1)
	{
		__m512i		d = _mm512_cvtepu8_epi32(b);

		IN_REG(d);
		return _mm512_cvtepi32_ps(_mm512_maskz_expand_epi32(k, d));
	}
	if (var == 2)
	{
		__m128i		lo = _mm_loadl_epi64((const __m128i *) lut8[k & 0xFF]),
					hi = _mm_add_epi8(_mm_loadl_epi64((const __m128i *) lut8[k >> 8]),
									  _mm_set1_epi8(__builtin_popcount(k & 0xFF)));

		return _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_shuffle_epi8(b, _mm_unpacklo_epi64(lo, hi))));
	}
	return _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_maskz_expand_epi8(k, b)));
}

static inline __attribute__((always_inline)) SIFT_TARGET_AVX512 int
tfvar(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
	  const float4 *knorm, float4 w, const int var)
{
	const __m512 vw = _mm512_set1_ps(w);
	int			r = 0;

	for (int i = 0; i < SIFT_KERN_WORDS(npages); i++)
		for (uint64 m = mask[i]; m;)
		{
			int			c = __builtin_ctzll(m) & ~15,
						s = i * 64 + c;
			uint32		kk = (uint16) (m >> c);
			__mmask16	k = kk;
			__m512		t = tf_lanes(tf + r, k, var);
			__m512		x = _mm512_maskz_div_ps(k, t, _mm512_maskz_add_ps(k, t, _mm512_load_ps(knorm + s)));

			_mm512_mask_store_ps(scores + s, k, _mm512_maskz_add_ps(k, _mm512_load_ps(scores + s), _mm512_mul_ps(vw, x)));
			r += __builtin_popcount(kk);
			m &= ~(UINT64CONST(0xFFFF) << c);
		}
	return r;
}

SIFT_TARGET_AVX512 int
alt_bm25_tfvar(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
			   const float4 *knorm, float4 w, int var)
{
	return var == 0 ? tfvar(scores, mask, npages, tf, knorm, w, 0) :
		var == 1 ? tfvar(scores, mask, npages, tf, knorm, w, 1) :
		tfvar(scores, mask, npages, tf, knorm, w, 2);
}
