/*
 * agg/gpu_dead.c - the mask of dead rows of a granule (agg/gpu_dead.h).
 */
#include "postgres.h"

#include "agg/gpu_dead.h"
#include "core/popcount.h"

uint32
sift_gpu_dead_count(const uint64 *mask)
{
	uint32		n = 0;

	for (int w = 0; w < SIFT_DV_MASK_WORDS; w++)
		n += (uint32) sift_pc64(mask[w]);
	return n;
}

uint32
sift_gpu_dead_add_tids(uint64 *mask, const uint64 *tids, uint32 nrows, const SiftPendDelIndex *px)
{
	for (uint32 r = 0; r < nrows; r++)
		if (sift_pdel_idx_has(px, tids[r]))
			mask[r >> 6] |= UINT64CONST(1) << (r & 63);
	return sift_gpu_dead_count(mask);
}
