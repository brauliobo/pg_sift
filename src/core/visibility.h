/*
 * core/visibility.h - visibility map masks and heap checks.
 *
 * The VM fork is read directly (2 bits per heap page, HEAPBLOCKS_PER_BYTE 4,
 * MAPSIZE = BLCKSZ - MAXALIGN(SizeOfPageHeaderData)); a regression test pins
 * this layout against visibilitymap_get_status().
 */
#ifndef SIFT_VISIBILITY_H
#define SIFT_VISIBILITY_H

#include "core/host.h"

#include "core/sift.h"

#define SIFT_VIS_DIRTY_MAX	8192

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
	int32		ndirty;			/* pages of the table that are not all-visible, listed in dirty (ascending); -1: not counted yet (0 after sift_vis_dirty_begin), -2: more than SIFT_VIS_DIRTY_MAX or no map */
	BlockNumber *dirty;
	MemoryContext mcxt;
	struct HTAB *pagevis;		/* block -> visibility of its rows, for pages visited twice (sift_vis_filter_page) */
	int64		heap_checks;	/* EXPLAIN counters */
	int64		heap_pages;
} SiftVis;

/* Blocks of the table in TID space (the relation size for a heap, the virtual blocks of a sift_table) */
extern BlockNumber sift_vis_nblocks(Relation heap);
/* The all-visible pages among the first nblocks of a heap, as visibilitymap_count counts them, reading only the map bytes they use */
extern BlockNumber sift_vis_count_allvisible(Relation heap, BlockNumber nblocks);
/*
 * index and dir_version are the index the scan reads and the version of the directory it read before it read any liveness bit (the plan's);
 * a caller that reads no liveness from an index (rows it takes from the heap pages themselves, or heap checks only) passes NULL and 0, and so does a column
 * scan, which asks the directory a finer question after the map (segments_current: is every segment it opened still there), so that a promotion,
 * which adds a segment, does not void the map for the rest of the scan.
 * A page is all-visible once a VACUUM has removed the index entries of its dead tuples, which clears liveness bits and then bumps the
 * directory version (sift_ambulkdelete): the liveness a scan read before that does not show the removals the map reflects.
 */
extern void sift_vis_begin(SiftVis *v, Relation heap, Snapshot snapshot, Relation index, uint32 dir_version);
extern void sift_vis_end(SiftVis *v);
/* releases the pins a check keeps (the visibility map page, the directory page, the heap page of a fetch); the next check takes them again */
extern void sift_vis_unpin(SiftVis *v);

/*
 * 256-bit mask of all-visible pages of an SB (all zero when !use_vm). The directory version is read after the map: once the directory has
 * changed since the scan read it, no page is all-visible, so the heap decides every candidate.
 */
extern void sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask);
/* Is every page in [lo, hi] all-visible? From the list of the pages that are not (made once from the map bytes); false when that list is too long. Same directory rule as sift_vis_sb_mask */
extern bool sift_vis_range_allvisible(SiftVis *v, BlockNumber lo, BlockNumber hi);
extern void sift_vis_blocks_allvisible(SiftVis *v, const BlockNumber *blocks, uint32 n, uint64 *bits);

#ifdef SIFT_TESTING
/* sift.test_vis_pause = N: the Nth mask of a scan waits for advisory lock (0x51F7, 1) before it reads the map; sift.test_vis_jitter_us sleeps up to that long */
extern PGDLLIMPORT int sift_test_vis_pause;
extern PGDLLIMPORT int sift_test_vis_jitter_us;
/* sift.test_count_gate: a count waits for lock (0x51F7, 1) once its plan is made; sift.test_cleanup_gate: a VACUUM waits for lock (0x51F7, 2) before its cleanup refreshes the dead-posting tables */
extern PGDLLIMPORT bool sift_test_count_gate;
extern PGDLLIMPORT bool sift_test_cleanup_gate;
extern void sift_test_wait(int key);
#define SIFT_TEST_GATE(flag, key) do { if (flag) sift_test_wait(key); } while (0)
#else
#define SIFT_TEST_GATE(flag, key) ((void) 0)
#endif

/*
 * Keeps only the offsets whose tuple (following its HOT chain) is visible
 * under the snapshot; reads the heap page once. Returns the remaining count.
 */
extern int	sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask);
/* A sift_table with HOT chains: the TIDs an index hands out are those of the first version of a chain, and the table's own scans see the later ones */
extern bool sift_vis_has_chains(Relation heap);
typedef void (*SiftVisKeyFn) (void *arg, SiftKey key);
extern void sift_vis_chain_members(Relation heap, BlockNumber blk, const uint64 *offmask, SiftVisKeyFn fn, void *arg);

/* The offset loop of a page filter: test says whether the tuple at tid is visible (the heap checks its HOT chain, a sift_table its row version) */
typedef bool (*SiftTidTest) (void *arg, ItemPointer tid);

/* Clears the bits of offmask (bit i: offset i + 1 of blk, SIFT_OFFMASK_WORDS words of 64) whose tid test rejects; returns the bits left */
extern int	sift_vis_filter_mask(BlockNumber blk, uint64 *offmask, SiftTidTest test, void *arg);

/* Fetches the visible version of tid into slot (tid becomes that version's, it differs along a HOT chain); false if none */
extern bool sift_vis_fetch(SiftVis *v, ItemPointer tid, TupleTableSlot *slot);

#endif							/* SIFT_VISIBILITY_H */
