/*
 * agg/kern6_sbbf.c - the slots of the granule bitmap, the dictionary pair and the split-block Bloom filter (agg/kern6.h) at every level, written
 * once: a slot is made of what its level already has. The filter itself is core/sbbf.c, the same code that the skipping indexes of the column
 * format call; its probe of a word runs at the level of the table, the range test of a key is the level's cmp_i64, the hash of a key its hash_i64,
 * and a granule mask is the level's mask_and in batches. The pair of dictionary codes is one loop that the compiler vectorizes for the
 * level it is compiled for (a vector level's function carries its target attribute). Results are bit-identical at every level.
 */
#include "core/host.h"

#include "agg/kern6_int.h"

static inline uint16
gran_and(const SiftAggKernelsV6 *t, uint64 *dst, const uint64 *a, const uint64 *b, uint16 n)
{
	uint32		pc = 0;

	for (uint32 from = 0; from < n; from += SIFT_VEC_LEN)
		pc += t->mask_and(dst + from / 64, a + from / 64, b + from / 64, (uint16) Min(SIFT_VEC_LEN, n - from));
	return (uint16) pc;
}

static inline uint32
pair_code(const uint32 *c, const uint64 *valid, uint32 null_code, uint32 r)
{
	return !valid || AK6_ROWBIT(valid, r) ? c[r] : null_code;
}

static inline __attribute__((always_inline)) void
pair_gid(uint32 *gid, const uint32 *c0, const uint64 *valid0, uint32 null0, const uint32 *c1, const uint64 *valid1, uint32 null1, uint32 dom1,
		 const uint16 *sel, uint16 n)
{
	if (sel)
		for (int i = 0; i < n; i++)
			gid[sel[i]] = pair_code(c0, valid0, null0, sel[i]) * dom1 + pair_code(c1, valid1, null1, sel[i]);
	else
		for (int r = 0; r < n; r++)
			gid[r] = pair_code(c0, valid0, null0, r) * dom1 + pair_code(c1, valid1, null1, r);
}

static inline uint16
sbbf_vec_i64(const SiftAggKernelsV6 *t, const SiftSbbfOps *ops, const SiftSbbfBlock *blocks, uint32 nblocks, int64 lo, int64 hi, const int64 *keys,
			 uint64 *io_mask, uint16 n)
{
	const int	nw = AK6_NW(n);
	uint32		left = 0;

	for (int w = 0; w < nw; w++)
	{
		const uint16 len = (uint16) Min(64, n - w * 64);
		uint64		ge,
					le,
					h[64];

		t->cmp_i64(&ge, keys + w * 64, len, SCMP_GE, lo);
		t->cmp_i64(&le, keys + w * 64, len, SCMP_LE, hi);
		io_mask[w] &= ge & le & AK6_TAIL(len);
		if (io_mask[w])
		{
			t->hash_i64(h, keys + w * 64, NULL, len, false);
			io_mask[w] = ops->probe_word(blocks, nblocks, h, io_mask[w]);
		}
		left += (uint32) sift_pc64(io_mask[w]);
	}
	return (uint16) left;
}

#define AK6_ADDED_LEVEL(lvl, table, level, attr) \
	uint16 ak6_mask_and_gran_##lvl(uint64 *dst, const uint64 *a, const uint64 *b, uint16 n) { return gran_and(&table, dst, a, b, n); } \
	attr void ak6_dict_pair_gid_##lvl(uint32 *gid, const uint32 *c0, const uint64 *valid0, uint32 null0, const uint32 *c1, const uint64 *valid1, \
									  uint32 null1, uint32 dom1, const uint16 *sel, uint16 n) \
	{ pair_gid(gid, c0, valid0, null0, c1, valid1, null1, dom1, sel, n); } \
	bool ak6_sbbf_probe_any_##lvl(const SiftSbbfBlock *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes) \
	{ return sift_sbbf_probe_any_at(sift_sbbf_ops_for(level), blocks, nblocks, hashes, nhashes); } \
	bool ak6_sbbf_probe_all_##lvl(const SiftSbbfBlock *blocks, uint32 nblocks, const uint64 *hashes, uint32 nhashes) \
	{ return sift_sbbf_probe_all_at(sift_sbbf_ops_for(level), blocks, nblocks, hashes, nhashes); } \
	uint16 ak6_sbbf_probe_vec_hash_##lvl(const SiftSbbfBlock *blocks, uint32 nblocks, const uint64 *hashes, uint64 *io_mask, uint16 n) \
	{ return sift_sbbf_probe_batch_at(sift_sbbf_ops_for(level), blocks, nblocks, hashes, io_mask, n); } \
	uint16 ak6_sbbf_probe_vec_i64_##lvl(const SiftSbbfBlock *blocks, uint32 nblocks, int64 min_key, int64 max_key, const int64 *keys, uint64 *io_mask, \
										uint16 n) \
	{ return sbbf_vec_i64(&table, sift_sbbf_ops_for(level), blocks, nblocks, min_key, max_key, keys, io_mask, n); }

AK6_ADDED_LEVEL(scalar, sift_aggkern6_scalar, SIFT_SIMD_SCALAR,)

#if defined(__x86_64__)
AK6_ADDED_LEVEL(avx2, sift_aggkern6_avx2, SIFT_SIMD_AVX2, SIFT_TARGET_AVX2)
AK6_ADDED_LEVEL(avx512, sift_aggkern6_avx512, SIFT_SIMD_AVX512, SIFT_TARGET_AVX512)
#elif defined(SIFT_HAVE_NEON)
AK6_ADDED_LEVEL(neon, sift_aggkern6_neon, SIFT_SIMD_NEON,)
#endif
