/*
 * sift_kernels.h - scoring SIMD kernels.
 *
 * One dispatch table per SIMD level, selected through sift_cpu_register().
 * Every kernel has a scalar reference; all levels must produce bit-identical
 * results (fixed operation order, no FMA, -ffp-contract=off). Mask and score
 * arrays are SIFT_SIMD_ALIGN aligned. Bitmap set operations (and/or/andnot/
 * popcount/iterate) belong to the codec module, not here.
 *
 * Offset-indexed arrays have SIFT_OFFMASK_WORDS * 64 slots; slot off-1 holds
 * heap offset off. Slots outside the given mask are left untouched.
 */
#ifndef SIFT_KERNELS_H
#define SIFT_KERNELS_H

#include "sift.h"
#include "sift_cpu.h"

typedef struct SiftKernels
{
	/*
	 * Rank -> offset space: out = pdep(rankbits, base) over nwords words;
	 * rankbits holds one bit per set bit of base, in order. Returns
	 * popcount(base), i.e. the number of rank bits consumed.
	 */
	int			(*expand) (uint64 *out, const uint64 *base, const uint64 *rankbits, int nwords);

	/*
	 * Decode a chunk's tf stream into tf[0..count): L1 bit
	 * per posting, a nibble per L1 member, an escape byte per nibble 15.
	 * Returns bytes consumed.
	 */
	size_t		(*tf_decode) (uint8 *tf, const uint8 *stream, uint32 count);

	/* knorm[o] = k1 * (1 - b + b * dl[o] / avgdl) for offsets in mask */
	void		(*knorm) (float4 *knorm, const uint16 *dl, const uint64 *mask,
						  float4 k1, float4 b, float4 avgdl);

	/*
	 * scores[o] += w * tf / (tf + knorm[o]) for offsets in mask, where tf is
	 * taken in rank order from tf[] (rank = position of o within mask) and
	 * w = boost * idf * (k1 + 1).
	 */
	void		(*bm25_accum) (float4 *scores, const uint64 *mask, const uint8 *tf,
							   const float4 *knorm, float4 w);

	/* scores[o] = max(scores[o], other[o]) for offsets in mask (dismax leaves) */
	void		(*max_merge) (float4 *scores, const float4 *other, const uint64 *mask);

	/* ub[i] += w * bounds[i] for i < n (u8 bounds widened to float) */
	void		(*ub_accum) (float4 *ub, const uint8 *bounds, float4 w, int n);

	/*
	 * Compress offsets in mask with scores[o] > theta into offs/vals.
	 * Returns the number written (<= SIFT_MAX_OFFSET).
	 */
	int			(*select_gt) (uint16 *offs, float4 *vals, const float4 *scores,
							  const uint64 *mask, float4 theta);

	/*
	 * Visibility map bytes for one SB (64 bytes, 2 bits per heap page) to a
	 * 256-bit all-visible page mask.
	 */
	void		(*vm_allvisible) (uint64 *pagemask, const uint8 *vmbytes);
} SiftKernels;

/* Active table; never NULL after _PG_init */
extern PGDLLIMPORT const SiftKernels *sift_kern;

/* Per-level tables, for equivalence tests; NULL when the CPU lacks the level */
extern const SiftKernels *sift_kernels_for(SiftSimdLevel level);

/* Registers the dispatch rebind; called from _PG_init */
extern void sift_kernels_init(void);

/* Exact BM25 term weight, the scalar definition every kernel must match */
static inline float4
sift_bm25(float4 w, float4 tf, float4 knorm)
{
	return w * (tf / (tf + knorm));
}

#endif							/* SIFT_KERNELS_H */
