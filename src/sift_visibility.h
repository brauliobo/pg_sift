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
	Relation	index;			/* the index whose directory guards the map (sift_vis_begin) */
	uint32		dir_version;	/* the version of the directory the scan read before it read any liveness */
	bool		dir_moved;		/* the directory changed since: the map is not trusted for the rest of the scan */
	Buffer		dirbuf;			/* the directory page, pinned at the first check */
	Buffer		vmbuf;
	BlockNumber vm_nblocks;
	BlockNumber heap_nblocks;
	struct IndexFetchTableData *fetch;
	int64		heap_checks;	/* EXPLAIN counters */
	int64		heap_pages;
} SiftVis;

/*
 * index and dir_version are the index the scan reads and the version of the directory it read before it read any liveness bit (the plan's).
 * A page is all-visible once a VACUUM has removed the index entries of its dead tuples, which clears liveness bits and then bumps the
 * directory version (sift_ambulkdelete): the liveness a scan read before that does not show the removals the map reflects.
 */
extern void sift_vis_begin(SiftVis *v, Relation heap, Snapshot snapshot, Relation index, uint32 dir_version);
extern void sift_vis_end(SiftVis *v);

/*
 * 256-bit mask of all-visible pages of an SB (all zero when !use_vm). The directory version is read after the map: once the directory has
 * changed since the scan read it, no page is all-visible, so the heap decides every candidate.
 */
extern void sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask);

#ifdef SIFT_TESTING
/* sift.test_vis_pause = N: the Nth mask of a scan waits for advisory lock (0x51F7, 1) before it reads the map; sift.test_vis_jitter_us sleeps up to that long */
extern PGDLLIMPORT int sift_test_vis_pause;
extern PGDLLIMPORT int sift_test_vis_jitter_us;
#endif

/*
 * Keeps only the offsets whose tuple (following its HOT chain) is visible
 * under the snapshot; reads the heap page once. Returns the remaining count.
 */
extern int	sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask);

/* Fetches the visible version of tid into slot (tid becomes that version's, it differs along a HOT chain); false if none */
extern bool sift_vis_fetch(SiftVis *v, ItemPointer tid, TupleTableSlot *slot);

#endif							/* SIFT_VISIBILITY_H */
