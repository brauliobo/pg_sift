/*
 * sift_kernels_avx512.c - AVX-512 (F/BW/VL/DQ/VBMI2) kernels. A 16-slot
 * chunk of a mask is one zmm of floats; only nonzero chunks are visited,
 * found two pages at a time with VPTESTMW. Rank-order tf bytes are placed
 * into slot lanes with PSHUFB from the AVX2 expand table. Register-form compress/expand only
 * (the memory forms are microcoded on Zen 4).
 */
#include "postgres.h"

#include "sift_kernels_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K512 static inline __attribute__((always_inline)) SIFT_TARGET_AVX512

/* Bitmap of the nonzero 16-slot chunks of pages p and p + 1 (if present) */
K512 uint64
chunks2(const uint64 *mask, int p, int npages)
{
	__m512i		a = _mm512_loadu_si512(mask + p * SIFT_OFFMASK_WORDS),
				b;
	uint64		nz = _mm512_test_epi16_mask(a, a);

	if (p + 1 == npages)
		return nz;
	b = _mm512_loadu_si512(mask + (p + 1) * SIFT_OFFMASK_WORDS);
	return nz | (uint64) _mm512_test_epi16_mask(b, b) << 32;
}

/* Visits the nonzero 16-slot chunks of mask: s = first slot, k = chunk bits */
#define FOREACH_CHUNK(mask, npages, s, k, body) \
	for (int p_ = 0; p_ < (npages); p_ += 2) \
		for (uint64 nz_ = chunks2(mask, p_, npages); nz_; nz_ &= nz_ - 1) \
		{ \
			int			s = p_ * SIFT_KERN_SLOTS + __builtin_ctzll(nz_) * 16; \
			uint32		k = (uint16) ((mask)[s >> 6] >> (s & 63)); \
			body; \
		}

/* tf of the chunk lanes: the next popcount(k) bytes of tf in lane order */
K512 __m512
chunk_tf(const uint8 *tf, uint32 k)
{
	__m128i		lo = _mm_loadl_epi64((const __m128i *) sift_kern_lut_expand[k & 0xFF]),
				hi = _mm_loadl_epi64((const __m128i *) sift_kern_lut_expand[k >> 8]);
	__m128i		ctl = _mm_unpacklo_epi64(lo, _mm_add_epi8(hi, _mm_set1_epi8(__builtin_popcount(k & 0xFF))));

	return _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *) tf), ctl)));
}

K512 int
bm25_accum_tmpl(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
				const float4 *knorm, float4 w, const bool tf1)
{
	const __m512 vw = _mm512_set1_ps(w),
				one = _mm512_set1_ps(1.0f);
	int			r = 0;

	FOREACH_CHUNK(mask, npages, s, k,
				  __m512 t = tf1 ? one : chunk_tf(tf + r, k);
				  __m512 x = _mm512_maskz_div_ps(k, t, _mm512_maskz_add_ps(k, t, _mm512_load_ps(knorm + s)));
				  __m512 sc = _mm512_maskz_add_ps(k, _mm512_load_ps(scores + s), _mm512_mul_ps(vw, x));
				  _mm512_mask_store_ps(scores + s, k, sc);
				  r +=__builtin_popcount(k));
	return r;
}

static SIFT_TARGET_AVX512 int
bm25_accum_avx512(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
				  const float4 *knorm, float4 w)
{
	return tf ? bm25_accum_tmpl(scores, mask, npages, tf, knorm, w, false) :
		bm25_accum_tmpl(scores, mask, npages, tf, knorm, w, true);
}

static SIFT_TARGET_AVX512 void
knorm_avx512(float4 *knorm, const uint16 *const *dl, const uint64 *mask, int npages,
			 const SiftBm25 *bm)
{
	const __m512 c0 = _mm512_set1_ps(bm->c0),
				c1 = _mm512_set1_ps(bm->c1);

	FOREACH_CHUNK(mask, npages, s, k,
				  __m256i d = _mm256_maskz_loadu_epi16(k, dl[s / SIFT_KERN_SLOTS] + s % SIFT_KERN_SLOTS);
				  __m512 f = _mm512_cvtepi32_ps(_mm512_cvtepu16_epi32(d));
				  _mm512_mask_store_ps(knorm + s, k, _mm512_add_ps(c0, _mm512_mul_ps(c1, f))));
}

static SIFT_TARGET_AVX512 void
max_merge_avx512(float4 *scores, const float4 *other, const uint64 *mask, int npages)
{
	FOREACH_CHUNK(mask, npages, s, k,
				  _mm512_mask_store_ps(scores + s, k, _mm512_max_ps(_mm512_load_ps(scores + s),
																	_mm512_load_ps(other + s))));
}

static SIFT_TARGET_AVX512 void
ub_accum_avx512(float4 *ub, const uint8 *bounds, float4 w, int n)
{
	const __m512 vw = _mm512_set1_ps(w);

	for (int i = 0; i < n; i += 16)
	{
		__mmask16	k = n - i >= 16 ? 0xFFFF : (__mmask16) ((1u << (n - i)) - 1);
		__m512		b = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(k, bounds + i)));

		_mm512_mask_storeu_ps(ub + i, k, _mm512_add_ps(_mm512_maskz_loadu_ps(k, ub + i), _mm512_mul_ps(vw, b)));
	}
}

static SIFT_TARGET_AVX512 int
select_gt_avx512(uint16 *slot, float4 *vals, const float4 *scores, const uint64 *mask,
				 int npages, float4 theta)
{
	const __m512 th = _mm512_set1_ps(theta);
	const __m256i iota = _mm256_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
	int			n = 0;

	FOREACH_CHUNK(mask, npages, s, k,
				  __m512 v = _mm512_load_ps(scores + s);
				  uint32 g = _mm512_mask_cmp_ps_mask(k, v, th, _CMP_GT_OQ);
				  if (g)
				  {
					  _mm512_storeu_ps(vals + n, _mm512_maskz_compress_ps(g, v));
					  _mm256_storeu_si256((__m256i *) (slot + n),
										  _mm256_maskz_compress_epi16(g, _mm256_add_epi16(iota, _mm256_set1_epi16(s))));
					  n += __builtin_popcount(g);
				  });
	return n;
}

/* The next 64 nibbles from nibble index nb, one per byte */
K512 __m512i
nibbles64(const uint8 *nib, uint32 nb)
{
	__m512i		q = _mm512_loadu_si512(nib + (nb >> 1));
	__m512i		x = _mm512_shrdv_epi64(q, _mm512_alignr_epi64(q, q, 1), _mm512_set1_epi64((nb & 1) * 4));
	__m512i		w = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(x));

	return _mm512_and_si512(_mm512_or_si512(w, _mm512_slli_epi16(w, 4)), _mm512_set1_epi16(0x0F0F));
}

static SIFT_TARGET_AVX512 void
tf_decode_avx512(uint8 *tf, const uint8 *l1, const uint8 *nib, const uint8 *esc,
				 SiftTfPos *pos, uint32 count)
{
	const __m512i one = _mm512_set1_epi8(1),
				two = _mm512_set1_epi8(2),
				x17 = _mm512_set1_epi8(17);
	uint32		nb = pos->nib,
				e = pos->esc;

	for (uint32 i = 0; i < count; i += 64)
	{
		uint64		bits = _bzhi_u64(sift_kern_bits64(l1, (uint64) pos->rank + i), Min(count - i, 64));
		__m512i		v = one;

		if (bits)
		{
			__mmask64	em;

			v = _mm512_mask_expand_epi8(one, bits, _mm512_add_epi8(nibbles64(nib, nb), two));
			nb += _mm_popcnt_u64(bits);
			em = _mm512_mask_cmpeq_epi8_mask(bits, v, x17);
			if (em)
			{
				v = _mm512_mask_expand_epi8(v, em, _mm512_adds_epu8(_mm512_loadu_si512(esc + e), x17));
				e += _mm_popcnt_u64(em);
			}
		}
		_mm512_storeu_si512(tf + i, v);
	}
	pos->rank += count;
	pos->nib = nb;
	pos->esc = e;
}

/* expand and vm_allvisible as AVX2: PDEP/PEXT are fast on every AVX-512 CPU */
const SiftKernels sift_kernels_avx512 = {
	.expand = sift_kern_expand_pdep,
	.tf_decode = tf_decode_avx512,
	.knorm = knorm_avx512,
	.bm25_accum = bm25_accum_avx512,
	.max_merge = max_merge_avx512,
	.ub_accum = ub_accum_avx512,
	.select_gt = select_gt_avx512,
	.vm_allvisible = sift_kern_vm_pext,
};

#endif
