/*
 * sift_visibility.h - visibility map masks and heap checks.
 *
 * The VM fork is read directly (2 bits per heap page, HEAPBLOCKS_PER_BYTE 4,
 * MAPSIZE = BLCKSZ - MAXALIGN(SizeOfPageHeaderData)); a regression test pins
 * this layout against visibilitymap_get_status().
 */
#ifndef SIFT_VISIBILITY_H
#define SIFT_VISIBILITY_H

#include "sift_host.h"

#include "sift.h"

typedef struct SiftVis
{
	Relation	heap;
	Snapshot	snapshot;
	bool		use_vm;			/* MVCC snapshot: all-visible pages skip heap checks */
	Buffer		vmbuf;
	BlockNumber vm_nblocks;
	BlockNumber heap_nblocks;
	struct IndexFetchTableData *fetch;
	int64		heap_checks;	/* EXPLAIN counters */
	int64		heap_pages;
} SiftVis;

/* Blocks of the table in TID space (the relation size for a heap, the virtual blocks of a sift_table) */
extern BlockNumber sift_vis_nblocks(Relation heap);
extern void sift_vis_begin(SiftVis *v, Relation heap, Snapshot snapshot);
extern void sift_vis_end(SiftVis *v);

/* 256-bit mask of all-visible pages of an SB (all zero when !use_vm) */
extern void sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask);

/*
 * Keeps only the offsets whose tuple (following its HOT chain) is visible
 * under the snapshot; reads the heap page once. Returns the remaining count.
 */
extern int	sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask);

/* The offset loop of a page filter: test says whether the tuple at tid is visible (the heap checks its HOT chain, a sift_table its row version) */
typedef bool (*SiftTidTest) (void *arg, ItemPointer tid);

/* Clears the bits of offmask (bit i: offset i + 1 of blk, SIFT_OFFMASK_WORDS words of 64) whose tid test rejects; returns the bits left */
extern int	sift_vis_filter_mask(BlockNumber blk, uint64 *offmask, SiftTidTest test, void *arg);

/* Fetches the visible version of tid into slot (tid becomes that version's, it differs along a HOT chain); false if none */
extern bool sift_vis_fetch(SiftVis *v, ItemPointer tid, TupleTableSlot *slot);

#endif							/* SIFT_VISIBILITY_H */
