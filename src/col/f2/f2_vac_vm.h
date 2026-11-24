/*
 * col/f2/f2_vac_vm.h - the heap blocks VACUUM may still find dead tuples in, from the visibility map of the heap (spec 49 5): ambulkdelete skips the parts and granules whose rows lie
 * in blocks the map says all-visible.
 *
 * Soundness: VACUUM never sets all-visible on a block that holds a dead item before ambulkdelete returns, and a concurrent change only clears the bit, so a block all-visible when the
 * set is built is a block the callback names no TID of. Only the public state of the map is read (the pages of its fork, under a share lock); the callback state is never touched.
 */
#ifndef SIFT_COL_F2_VAC_VM_H
#define SIFT_COL_F2_VAC_VM_H

#include "postgres.h"

#include "storage/block.h"

typedef struct SiftBlkInterval
{
	BlockNumber lo;				/* inclusive */
	BlockNumber hi;				/* inclusive */
} SiftBlkInterval;

typedef struct SiftVmDirtySet
{
	BlockNumber nheap_blocks;	/* the blocks of the heap when the set was built; any block past it is dirty */
	SiftBlkInterval *intervals;	/* sorted, disjoint, coalesced runs of the blocks that are not all-visible */
	int			nintervals;
	int			cap;
	uint64		dirty_blocks;
	bool		all_dirty;		/* nothing can be skipped: no heap of the standard access method, or more than half of its blocks are dirty */
} SiftVmDirtySet;

/* 2 bits for each heap block in the map: bit 0 all-visible, bit 1 all-frozen; one 64-bit word is 32 blocks */
#define SIFT_VM_BLOCKS_PER_WORD		32
#define SIFT_VM_ALL_VISIBLE_BITS	UINT64CONST(0x5555555555555555)
#define SIFT_VM_DIRTY_MAX_BLOOM_BLOCKS	64	/* a range with more dirty blocks than this is not tested against the bloom of a granule */
#define SIFT_VM_DIRTY_MAX_INTERVALS		65536	/* a set of more runs than this (512 KB) is the set that skips nothing: no memory follows the size of the heap */

/* The bloom bit of a heap block, as SEC_TID_INDEX keeps it (sift_tid_block_bit of an address in the block) */
static inline uint64
sift_vm_blk_bit(BlockNumber blk)
{
	return UINT64CONST(1) << ((blk * UINT64CONST(0x9E3779B97F4A7C15)) >> 58);
}

/* An empty set for a heap of nheap_blocks blocks: no block is dirty yet, and a block past them always is */
extern void sift_vm_dirty_init(SiftVmDirtySet *ds, BlockNumber nheap_blocks);

/* The set that skips nothing */
extern void sift_vm_dirty_all(SiftVmDirtySet *ds);

/* The words of the map for the blocks [base, base + 32 * nwords): the blocks whose all-visible bit is clear are added; base is a multiple of 32 */
extern void sift_vm_dirty_scan_words(SiftVmDirtySet *ds, BlockNumber base, const uint64 *words, int nwords);

/* The set is complete: more than half of the blocks dirty turns the interval search off, and its runs are freed */
extern void sift_vm_dirty_finish(SiftVmDirtySet *ds);

extern void sift_vm_dirty_free(SiftVmDirtySet *ds);

/* Whether any block of [lo, hi] may hold a dead item: false is the proof that none does */
extern bool sift_vm_dirty_overlaps(const SiftVmDirtySet *ds, BlockNumber lo, BlockNumber hi);

/* The same for a granule whose rows lie in the blocks of bloom (a bit for each, sift_vm_blk_bit): a dirty block of the range that the granule has no bit for is not a reason to read it */
extern bool sift_vm_dirty_overlaps_bloom(const SiftVmDirtySet *ds, BlockNumber lo, BlockNumber hi, uint64 bloom);

#ifndef SIFT_S49_STANDALONE
#include "utils/rel.h"

/* The set of the heap the vacuum is of; the set that skips nothing for a relation of another access method or a concurrent build's validation (report_progress) */
extern void sift_vm_dirty_build(SiftVmDirtySet *ds, Relation heap, bool validating);
#endif

#endif							/* SIFT_COL_F2_VAC_VM_H */
