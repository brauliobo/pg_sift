/*
 * sift_visibility.h - visibility map masks and heap checks.
 *
 * The VM fork is read directly (2 bits per heap page, HEAPBLOCKS_PER_BYTE 4,
 * MAPSIZE = BLCKSZ - MAXALIGN(SizeOfPageHeaderData)); a regression test pins
 * this layout against visibilitymap_get_status().
 */
#ifndef SIFT_VISIBILITY_H
#define SIFT_VISIBILITY_H

#include "executor/tuptable.h"
#include "utils/snapshot.h"

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

extern void sift_vis_begin(SiftVis *v, Relation heap, Snapshot snapshot);
extern void sift_vis_end(SiftVis *v);

/* 256-bit mask of all-visible pages of an SB (all zero when !use_vm) */
extern void sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask);

/*
 * Keeps only the offsets whose tuple (following its HOT chain) is visible
 * under the snapshot; reads the heap page once. Returns the remaining count.
 */
extern int	sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask);

/* Fetches the visible version of tid into slot (tid becomes that version's, it differs along a HOT chain); false if none */
extern bool sift_vis_fetch(SiftVis *v, ItemPointer tid, TupleTableSlot *slot);

#endif							/* SIFT_VISIBILITY_H */
