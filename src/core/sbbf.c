/*
 * core/sbbf.c - the split-block Bloom filter at every SIMD level (core/sbbf.h): the scalar functions are the specification; AVX2 tests a hash with
 * one 256-bit multiply, shift and VPTEST, AVX-512 two hashes at a time in one register, NEON with two 128-bit halves. The batch operations are made of the
 * probe of a word of 64 rows, which visits the set bits of the row mask only: a word with no live row costs nothing.
 */
#include "core/host.h"

#include <math.h>

#include "core/hash.h"
#include "core/popcount.h"
#include "core/sbbf.h"

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(SIFT_HAVE_NEON)
#include <arm_neon.h>
#endif

/* ---- hashes ---- */

uint64
sift_sbbf_hash_i64(int64 v)
{
	return sift_fmix64((uint64) v);
}

uint64
sift_sbbf_hash_bytes(const void *p, uint32 len)
{
	const uint8 *s = p;
	uint64		h = SIFT_FIB64 * ((uint64) len + 1);
	uint32		i = 0;

	for (; i + 8 <= len; i += 8)
	{
		uint64		w;

		memcpy(&w, s + i, 8);
		h = sift_fold64(h, w);
	}
	if (i < len)
	{
		uint64		w = 0;

		memcpy(&w, s + i, len - i);
		h = sift_fold64(h, w);
	}
	return sift_fmix64(h);
}

/* ---- scalar ---- */

static inline uint8 *
block_at(void *blocks, uint64 hash, uint32 nblocks)
{
	return (uint8 *) blocks + (size_t) sift_sbbf_block_of(hash, nblocks) * SIFT_SBBF_BLOCK_BYTES;
}

void
sift_sbbf_insert_scalar(void *blocks, uint32 nblocks, uint64 hash)
{
	uint8	   *b = block_at(blocks, hash, nblocks);

	for (int j = 0; j < 8; j++)
	{
		uint32		w;

		memcpy(&w, b + 4 * j, 4);
		w |= sift_sbbf_bit(hash, j);
		memcpy(b + 4 * j, &w, 4);
	}
}

bool
sift_sbbf_probe_scalar(const void *blocks, uint32 nblocks, uint64 hash)
{
	const uint8 *b = block_at((void *) blocks, hash, nblocks);

	for (int j = 0; j < 8; j++)
	{
		uint32		w;

		memcpy(&w, b + 4 * j, 4);
		if (!(w & sift_sbbf_bit(hash, j)))
			return false;
	}
	return true;
}

static void
insert_scalar(void *blocks, uint32 nblocks, uint64 hash)
{
	sift_sbbf_insert_scalar(blocks, nblocks, hash);
}

static uint64
probe_word_scalar(const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 mask)
{
	uint64		hit = 0;

	for (uint64 m = mask; m; m &= m - 1)
	{
		const int	i = __builtin_ctzll(m);

		hit |= (uint64) sift_sbbf_probe_scalar(blocks, nblocks, hashes[i]) << i;
	}
	return hit;
}

const SiftSbbfOps sift_sbbf_ops_scalar = {insert_scalar, probe_word_scalar};

/* ---- AVX2 ---- */

#if defined(__x86_64__)

SIFT_TARGET_AVX2 static inline __m256i
mask_avx2(uint64 hash)
{
	const __m256i salt = _mm256_loadu_si256((const __m256i *) SIFT_SBBF_SALT);
	const __m256i sh = _mm256_srli_epi32(_mm256_mullo_epi32(_mm256_set1_epi32((int) (uint32) hash), salt), 27);

	return _mm256_sllv_epi32(_mm256_set1_epi32(1), sh);
}

SIFT_TARGET_AVX2 static void
insert_avx2(void *blocks, uint32 nblocks, uint64 hash)
{
	__m256i    *b = (__m256i *) block_at(blocks, hash, nblocks);

	_mm256_storeu_si256(b, _mm256_or_si256(_mm256_loadu_si256(b), mask_avx2(hash)));
}

SIFT_TARGET_AVX2 static uint64
probe_word_avx2(const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 mask)
{
	uint64		hit = 0;

	for (uint64 m = mask; m; m &= m - 1)
	{
		const int	i = __builtin_ctzll(m);
		const uint64 h = hashes[i];
		const __m256i blk = _mm256_loadu_si256((const __m256i *) block_at((void *) blocks, h, nblocks));

		hit |= (uint64) (_mm256_testc_si256(blk, mask_avx2(h)) != 0) << i;
	}
	return hit;
}

const SiftSbbfOps sift_sbbf_ops_avx2 = {insert_avx2, probe_word_avx2};

/* ---- AVX-512: the masks of two hashes in the two halves of one register ---- */

SIFT_TARGET_AVX512 static inline __m512i
mask2_avx512(uint64 h0, uint64 h1)
{
	const __m512i salt = _mm512_broadcast_i32x8(_mm256_loadu_si256((const __m256i *) SIFT_SBBF_SALT));
	const __m512i hv = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set1_epi32((int) (uint32) h0)),
										  _mm256_set1_epi32((int) (uint32) h1), 1);

	return _mm512_sllv_epi32(_mm512_set1_epi32(1), _mm512_srli_epi32(_mm512_mullo_epi32(hv, salt), 27));
}

SIFT_TARGET_AVX512 static void
insert_avx512(void *blocks, uint32 nblocks, uint64 hash)
{
	__m256i    *b = (__m256i *) block_at(blocks, hash, nblocks);

	_mm256_storeu_si256(b, _mm256_or_si256(_mm256_loadu_si256(b), _mm512_castsi512_si256(mask2_avx512(hash, hash))));
}

SIFT_TARGET_AVX512 static uint64
probe_word_avx512(const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 mask)
{
	uint64		hit = 0;
	uint64		m = mask;

	while (m)
	{
		const int	i = __builtin_ctzll(m);
		const uint64 h0 = hashes[i];
		const uint64 rest = m & (m - 1);

		if (rest)
		{
			const int	j = __builtin_ctzll(rest);
			const uint64 h1 = hashes[j];
			const __m512i blk = _mm512_inserti64x4(
													_mm512_castsi256_si512(_mm256_loadu_si256((const __m256i *) block_at((void *) blocks, h0, nblocks))),
													_mm256_loadu_si256((const __m256i *) block_at((void *) blocks, h1, nblocks)), 1);
			const __m512i missing = _mm512_andnot_si512(blk, mask2_avx512(h0, h1));
			const uint32 lanes = (uint32) _mm512_test_epi32_mask(missing, missing);

			hit |= (uint64) ((lanes & 0xFF) == 0) << i;
			hit |= (uint64) ((lanes >> 8) == 0) << j;
			m = rest & (rest - 1);
		}
		else
		{
			const __m256i blk = _mm256_loadu_si256((const __m256i *) block_at((void *) blocks, h0, nblocks));

			hit |= (uint64) (_mm256_testc_si256(blk, _mm512_castsi512_si256(mask2_avx512(h0, h0))) != 0) << i;
			m = rest;
		}
	}
	return hit;
}

const SiftSbbfOps sift_sbbf_ops_avx512 = {insert_avx512, probe_word_avx512};

#endif							/* __x86_64__ */

/* ---- NEON ---- */

#if defined(SIFT_HAVE_NEON)

static inline void
mask_neon(uint64 hash, uint32x4_t *lo, uint32x4_t *hi)
{
	const uint32x4_t hv = vdupq_n_u32((uint32) hash);
	const uint32x4_t one = vdupq_n_u32(1);

	*lo = vshlq_u32(one, vreinterpretq_s32_u32(vshrq_n_u32(vmulq_u32(hv, vld1q_u32(SIFT_SBBF_SALT)), 27)));
	*hi = vshlq_u32(one, vreinterpretq_s32_u32(vshrq_n_u32(vmulq_u32(hv, vld1q_u32(SIFT_SBBF_SALT + 4)), 27)));
}

static void
insert_neon(void *blocks, uint32 nblocks, uint64 hash)
{
	uint8	   *b = block_at(blocks, hash, nblocks);
	uint32x4_t	lo,
				hi;

	mask_neon(hash, &lo, &hi);
	vst1q_u8(b, vreinterpretq_u8_u32(vorrq_u32(vreinterpretq_u32_u8(vld1q_u8(b)), lo)));
	vst1q_u8(b + 16, vreinterpretq_u8_u32(vorrq_u32(vreinterpretq_u32_u8(vld1q_u8(b + 16)), hi)));
}

static uint64
probe_word_neon(const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 mask)
{
	uint64		hit = 0;

	for (uint64 m = mask; m; m &= m - 1)
	{
		const int	i = __builtin_ctzll(m);
		const uint8 *b = block_at((void *) blocks, hashes[i], nblocks);
		uint32x4_t	lo,
					hi;

		mask_neon(hashes[i], &lo, &hi);
		hit |= (uint64) (vmaxvq_u32(vorrq_u32(vbicq_u32(lo, vreinterpretq_u32_u8(vld1q_u8(b))), vbicq_u32(hi, vreinterpretq_u32_u8(vld1q_u8(b + 16))))) == 0) << i;
	}
	return hit;
}

const SiftSbbfOps sift_sbbf_ops_neon = {insert_neon, probe_word_neon};

#endif							/* SIFT_HAVE_NEON */

const SiftSbbfOps *
sift_sbbf_ops_for(SiftSimdLevel level)
{
#if defined(SIFT_HAVE_NEON)
	if (level >= SIFT_SIMD_NEON && sift_cpu.detected >= SIFT_SIMD_NEON)
		return &sift_sbbf_ops_neon;
#endif
#if defined(__x86_64__)
	if (level >= SIFT_SIMD_AVX512 && sift_cpu.detected >= SIFT_SIMD_AVX512)
		return &sift_sbbf_ops_avx512;
	if (level >= SIFT_SIMD_AVX2 && sift_cpu.detected >= SIFT_SIMD_AVX2)
		return &sift_sbbf_ops_avx2;
#endif
	return &sift_sbbf_ops_scalar;
}

/* ---- the operations made of them ---- */

void
sift_sbbf_insert(void *blocks, uint32 nblocks, uint64 hash)
{
	sift_sbbf_ops_for(sift_cpu.active)->insert(blocks, nblocks, hash);
}

void
sift_sbbf_insert_atomic(void *blocks, uint32 nblocks, uint64 hash)
{
	uint32	   *w = (uint32 *) block_at(blocks, hash, nblocks);

	for (int j = 0; j < 8; j++)
	{
		const uint32 bit = sift_sbbf_bit(hash, j);

		if ((__atomic_load_n(&w[j], __ATOMIC_RELAXED) & bit) != bit)
			__atomic_fetch_or(&w[j], bit, __ATOMIC_RELAXED);
	}
}

uint16
sift_sbbf_probe_batch_at(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 *io_mask, uint16 n)
{
	const int	nw = (n + 63) >> 6;
	uint32		left = 0;

	for (int w = 0; w < nw; w++)
	{
		const uint64 live = (w == nw - 1 && (n & 63)) ? io_mask[w] & ((UINT64CONST(1) << (n & 63)) - 1) : io_mask[w];

		io_mask[w] = live ? ops->probe_word(blocks, nblocks, hashes + (size_t) w * 64, live) : 0;
		left += (uint32) sift_pc64(io_mask[w]);
	}
	return (uint16) left;
}

uint16
sift_sbbf_probe_batch(const void *blocks, uint32 nblocks, const uint64 *hashes, uint64 *io_mask, uint16 n)
{
	return sift_sbbf_probe_batch_at(sift_sbbf_ops_for(sift_cpu.active), blocks, nblocks, hashes, io_mask, n);
}

/* ops->probe_word over the hashes in groups of 64: every group is tested whole, the answer of the group is its count of hits against its size */
static bool
probe_groups(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes, bool all)
{
	for (uint32 from = 0; from < nhashes; from += 64)
	{
		const uint32 len = Min(64, nhashes - from);
		const uint64 full = len == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << len) - 1;
		const uint64 hit = ops->probe_word(blocks, nblocks, hashes + from, full);

		if (all ? hit != full : hit != 0)
			return !all;
	}
	return all;
}

bool
sift_sbbf_probe_any_at(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes)
{
	return probe_groups(ops, blocks, nblocks, hashes, nhashes, false);
}

bool
sift_sbbf_probe_all_at(const SiftSbbfOps *ops, const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes)
{
	return probe_groups(ops, blocks, nblocks, hashes, nhashes, true);
}

bool
sift_sbbf_probe(const void *blocks, uint32 nblocks, uint64 hash)
{
	return sift_sbbf_ops_for(sift_cpu.active)->probe_word(blocks, nblocks, &hash, 1) != 0;
}

bool
sift_sbbf_probe_any(const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 n)
{
	return probe_groups(sift_sbbf_ops_for(sift_cpu.active), blocks, nblocks, hashes, n, false);
}

bool
sift_sbbf_probe_all(const void *blocks, uint32 nblocks, const uint64 *hashes, uint32 n)
{
	return probe_groups(sift_sbbf_ops_for(sift_cpu.active), blocks, nblocks, hashes, n, true);
}

uint32
sift_sbbf_nblocks(uint32 ndistinct, uint32 bits_per_key)
{
	return sift_sbbf_blocks_for(ndistinct, bits_per_key);
}

uint32
sift_sbbf_blocks_for(uint64 n_keys, uint32 bits_per_key)
{
	const uint64 nb = (n_keys * bits_per_key + 255) / 256;

	return (uint32) Min(Max(nb, 1), UINT32_MAX);
}

/* sum over k of P(k keys in a block) * P(a probe finds its eight bits set | k keys): each word has a bit set with probability 1 - (31/32)^k */
double
sift_sbbf_fpr(uint64 n_keys, uint32 nblocks)
{
	const double lambda = (double) n_keys / nblocks;
	const double sd = sqrt(lambda);
	const int	lo = (int) Max(0, lambda - 12 * sd - 20),
				hi = (int) (lambda + 12 * sd + 20);
	double		fpr = 0;

	if (n_keys == 0)
		return 0;
	for (int k = lo; k <= hi; k++)
	{
		const double pk = exp(k * log(lambda) - lambda - lgamma(k + 1.0));
		const double fill = 1 - exp(k * log1p(-1.0 / 32));
		const double fill8 = fill * fill * fill * fill * fill * fill * fill * fill;

		fpr += pk * fill8;
	}
	return Min(fpr, 1.0);
}
