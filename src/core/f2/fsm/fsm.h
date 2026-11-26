/*
 * core/fsm.h - the allocator of format major 2: power-of-two size classes over a data zone, a bitmap slab for the one-block metadata pages, and a deferred-reuse queue.
 *
 * Block 0 is the metapage, block 1 the root of this allocator (SIFT_PT_FSM_META) and [2, meta_slab_end) the first metadata slab, whose first block is the root of
 * the partition directory at the start: the one-block pages of the manifest, the part headers and the lists of the allocator itself come from the bitmaps of the slabs, so that the copy on write of
 * a manifest never pins the end of the file. Everything after it is the data zone, handed out in extents of 1, 4, 16, 64, 256 or 1024 blocks.
 *
 * The free extents of a class are a stack of SIFT_PT_FSM_PAGE pages: an allocation pops the last entry of the head page (the head and the root are the only pages it
 * writes), a release is a push on the queue of released extents. An extent released goes to that queue with the transaction that released it (P4): it is reusable only once the
 * horizon has passed it, which sift_fsm_reclaim finds out, merging the matured extents with the free ones, coalescing adjacent runs and cutting them into classes again.
 * All of it runs under the exclusive lock of block 0 (the order of the locks is block 0, block 1, the pages of the lists), so a record of generic WAL here never holds more than
 * the 4 pages the logger allows.
 */
#ifndef SIFT_FSM_H
#define SIFT_FSM_H

#include "core/host.h"

#include "core/f2/meta.h"
#include "core/xlog.h"

#define SIFT_FSM_MAGIC			0x46534D32U		/* "FSM2" */
#define SIFT_FSM_VERSION		1
#define SIFT_FSM_NCLASSES		11
#define SIFT_FSM_POOL_SPAN		32				/* the blocks of the window of a batch of metadata pages: a bit of pool_mask for each */
#define SIFT_FSM_CLASS_BLOCKS(c) (1U << (c))
#define SIFT_FSM_MAX_EXTENT		SIFT_FSM_CLASS_BLOCKS(SIFT_FSM_NCLASSES - 1)
#define SIFT_FSM_META_SLAB		256				/* the most blocks of a metadata slab, the first one included */
#define SIFT_FSM_SLAB_FIRST		8				/* the blocks of the first and of the second slab: each next one is twice as big, up to SIFT_FSM_META_SLAB, so that a small index reserves little */
#define SIFT_FSM_SLAB_GROWTH	5				/* the slabs that double: SIFT_FSM_SLAB_FIRST << SIFT_FSM_SLAB_GROWTH == SIFT_FSM_META_SLAB */
StaticAssertDecl((SIFT_FSM_SLAB_FIRST << SIFT_FSM_SLAB_GROWTH) == SIFT_FSM_META_SLAB, "the slabs grow to the largest");

typedef struct SiftFsmMetaPageData
{
	uint32		magic;
	uint16		version;
	uint16		flags;
	uint32		crc32c;
	BlockNumber pool_lo;		/* the first block of the window of the last batch of metadata pages leased to a pool (core/shm/lease.h); 0: none */
	BlockNumber class_head[SIFT_FSM_NCLASSES];			/* the head SIFT_PT_FSM_PAGE of the free extents of each class, or InvalidBlockNumber */
	uint32		class_free_extents[SIFT_FSM_NCLASSES];
	uint32		swept_start;	/* the start time of the server that swept the orphans last (core/f2/fsm/sweep.c); 0: never */
	uint32		meta_free_count;	/* free blocks over all slabs */
	BlockNumber quarantine_head;	/* oldest page of the queue of released extents */
	BlockNumber quarantine_tail;	/* the page that takes the next one */
	uint32		quarantine_extents;
	uint32		pool_mask;		/* a bit for each block of that window that the batch took: a page of it that nothing names belongs to a pool that a restart or a crash lost, and the orphan sweep frees it */
	uint64		quarantine_blocks;
} SiftFsmMetaPageData;

StaticAssertDecl(sizeof(SiftFsmMetaPageData) == 136, "SiftFsmMetaPageData must be 136 bytes");

/* A metadata slab: the blocks [start, start + nblocks) with a bit per block, set when the block is free */
typedef struct SiftFsmSlab
{
	BlockNumber start;
	uint32		nblocks;
	uint64		free[SIFT_FSM_META_SLAB / 64];
} SiftFsmSlab;

StaticAssertDecl(sizeof(SiftFsmSlab) == 40, "SiftFsmSlab must be 40 bytes");

typedef struct SiftFsmSlabTable
{
	uint32		nslabs;
	uint32		pad;
	SiftFsmSlab slab[FLEXIBLE_ARRAY_MEMBER];
} SiftFsmSlabTable;

#define SIFT_FSM_SLABS_OFF		sizeof(SiftFsmMetaPageData)
#define SIFT_FSM_MAX_SLABS		((SIFT_PAGE_PAYLOAD - SIFT_FSM_SLABS_OFF - offsetof(SiftFsmSlabTable, slab)) / sizeof(SiftFsmSlab))

#define SIFT_FSM_PAGE_DEFERRED	(UINT16_C(1) << 0)
#define SIFT_FSM_CLASS_QUEUE	SIFT_FSM_NCLASSES		/* size_class of a page of the queue of released extents */

typedef struct SiftFsmExtent
{
	BlockNumber start;
	uint32		nblocks;
	FullTransactionId safexid;	/* the transaction that released it; invalid: reusable at once */
} SiftFsmExtent;

StaticAssertDecl(sizeof(SiftFsmExtent) == 16, "SiftFsmExtent must be 16 bytes");

typedef struct SiftFsmPageData
{
	uint32		magic;
	uint16		flags;			/* SIFT_FSM_PAGE_DEFERRED: a page of the queue */
	uint16		size_class;
	uint32		crc32c;
	uint32		nentries;
	BlockNumber next_blk;
	uint32		pad;
	SiftFsmExtent ext[FLEXIBLE_ARRAY_MEMBER];
} SiftFsmPageData;

StaticAssertDecl(offsetof(SiftFsmPageData, ext) == 24, "SiftFsmPageData header is 24 bytes");

#define SIFT_FSM_PAGE_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftFsmPageData, ext)) / sizeof(SiftFsmExtent))

/* The root page (block 1) of an index being created: no free extent, nothing queued, and the first metadata slab [slab_start, slab_start + SIFT_FSM_SLAB_FIRST) free but for its first block (the root of the partition directory) */
extern void sift_fsm_build_root(Page page, BlockNumber slab_start);

/* The blocks of the slab after nslabs others: a power of two, so that a free extent of its class can serve as the slab */
#define SIFT_FSM_SLAB_BLOCKS(nslabs) (SIFT_FSM_SLAB_FIRST << Min((nslabs) > 0 ? (nslabs) - 1 : 0, SIFT_FSM_SLAB_GROWTH))

/*
 * One extent of exactly nblocks (at most 1024): cut off the front of a free extent that holds it, whose rest goes back to the lists, or cut from the end of the relation. Logged before the caller
 * writes a block of it (P3).
 */
extern SiftExtent sift_fsm_alloc(Relation rel, uint32 nblocks);

/* npages as power-of-two extents that all lie below block max_blk, lowest first (what a part moved down needs); the number of extents, or 0 when the free space below max_blk does not hold them (the file never grows for it) */
extern int	sift_fsm_alloc_below(Relation rel, uint64 npages, BlockNumber max_blk, SiftExtent *out, int max);

/*
 * The extents of a stream of exactly nblocks blocks: while a free extent holds the largest power of two that is left they are those powers of two (at most popcount of them below
 * 1024, then 1024-block ones), the rest one extent cut from the end of the relation; returns how many of the max it wrote
 */
extern int	sift_fsm_alloc_stream(Relation rel, uint64 nblocks, SiftExtent *out, int max);

/*
 * One page of metadata, from the bitmaps of the slabs (a new slab is taken from the data zone when all are full): from the pool of pages that writers of the file took in advance (core/shm/lease.h) when
 * it has one, which takes no exclusive lock of block 0 and one record of the root for a batch of them
 */
extern BlockNumber sift_fsm_alloc_meta_page(Relation rel);

/* The pages of the pool of the file go back to the slabs: before whoever looks for the pages that nothing names (the sweep of the orphans, the check of the blocks) */
extern void sift_fsm_meta_pool_drain(Relation rel);

/* Queues the extents for reuse once safexid is past the horizon (an invalid safexid: at once); a one-block extent of a slab goes back to its bitmap at that time */
extern void sift_fsm_release(Relation rel, const SiftExtent *ext, int n, FullTransactionId safexid);

/*
 * The same inside the record of a manifest commit (P2): the caller holds block 0 exclusively and has registered it in x; sift_fsm_reserve_release (given that buffer) first makes sure the tail of the
 * queue has room for n <= SIFT_FSM_PAGE_CAP extents (its own record), sift_fsm_release_in adds them to x (the root and the tail page, no other).
 */
extern void sift_fsm_reserve_release(Relation rel, Buffer buf0, int n);
extern void sift_fsm_release_in(Relation rel, SiftXlog *x, const SiftExtent *ext, int n, FullTransactionId safexid);

/* Moves the extents whose safexid has passed the horizon to the free lists, coalesced and cut into classes again; returns the blocks that became free */
extern uint64 sift_fsm_reclaim(Relation rel);

/* The same for a caller that holds AccessExclusiveLock on the relation: no reader of an older manifest exists, so every extent of the queue is reclaimed; no empty slab is given back (the caller is filling the slabs it made) */
extern uint64 sift_fsm_reclaim_exclusive(Relation rel);

/*
 * Gives the extents that nothing names back to the allocator: what a crash, or a failure after the write of a part and before the commit that names it, leaves allocated. It has the relation to itself
 * (AccessExclusiveLock taken without waiting unless held, the load lock, no VACUUM reading the manifest; never while a standby streams) and frees nothing, DEBUG1 says why, when the relation holds
 * what it cannot name (core/f2/fsm/sweep.c); the blocks freed. sift_fsm_tail_shrink, which every VACUUM ends with, runs it once for each relation and server start.
 */
extern uint64 sift_fsm_sweep_orphans(Relation rel);

/* Whether this server has not swept the orphans of the relation yet: until it has, what a crash left allocated is not a leak */
extern bool sift_fsm_sweep_due(Relation rel);

/* Whether every extent of the relation has a name that a pass over its parts finds: a sift_table keeps its wide values in extents that only a granule or a micro-slab names, so a table with a column of variable length has not */
extern bool sift_fsm_names_all_extents(Relation rel);

typedef struct SiftFsmStats
{
	uint64		class_blocks[SIFT_FSM_NCLASSES];
	uint32		class_extents[SIFT_FSM_NCLASSES];
	uint64		meta_free;
	uint64		meta_total;
	uint64		meta_idle;		/* the blocks of the slabs with no page in use that can be given back */
	uint64		quarantine_blocks;
	uint64		tail_blocks;	/* the free and queued blocks that end at the high-water mark: what a cut of the file can give back */
	uint32		quarantine_extents;
	uint32		pages;			/* blocks held by the lists themselves */
} SiftFsmStats;

/* Walks every list and checks it (counts, magics, checksums, no block twice); ERROR when the allocator is damaged */
extern SiftFsmStats sift_fsm_check(Relation rel);

/*
 * The free run at the end of the file given back: the matured extents are reclaimed (under the lock of the cut, the quarantined ones too: no reader of an older manifest can be there), and when at least SIFT_TAIL_SHRINK_FREE_PCT of the blocks are free (and at least
 * SIFT_TAIL_SHRINK_MIN_BLOCKS of them) the run that ends at the high-water mark is left out of the lists and the mark lowered to its start in one record, then the file is cut (P7). While a standby streams
 * nothing is cut: a standby replays the lock of the cut and cancels the queries that hold the relation (lock_skipped).
 * The cut tries AccessExclusiveLock on the index without ever queueing for it, every SIFT_TAIL_SHRINK_WAIT_MS ms: SIFT_TAIL_SHRINK_FOREGROUND_TRIES times in a VACUUM of a session,
 * SIFT_TAIL_SHRINK_BACKGROUND_TRIES times in autovacuum and in the workers of sift. Still busy, nothing is cut and lock_skipped says so (a later pass tries again).
 */
#define SIFT_TAIL_SHRINK_FREE_PCT	3
#define SIFT_TAIL_SHRINK_MIN_BLOCKS	4
#define SIFT_TAIL_SHRINK_WAIT_MS	5
#define SIFT_TAIL_SHRINK_FOREGROUND_TRIES	50
#define SIFT_TAIL_SHRINK_BACKGROUND_TRIES	10

typedef struct SiftTailShrinkResult
{
	uint64		blocks_truncated;
	bool		lock_skipped;
} SiftTailShrinkResult;

extern SiftTailShrinkResult sift_fsm_tail_shrink(Relation rel);

/* The free blocks this backend holds for the relation file (the extents a move of a part prefers) are dropped with the file: set_new_filelocator and TRUNCATE (core/f2/fsm/cache.c) */
extern void sift_fsm_cache_discard_rel(Relation rel);

/* Is the block one of a metadata slab */
extern bool sift_fsm_is_meta_block(Relation rel, BlockNumber blk);

/*
 * The pages in use of the metadata slabs that start at or above from that the free pages of the slabs below it do not hold (0: all fit): a commit that rewrites a page takes the lowest free one.
 * for_move: the pages that the commits which move them write, and the reserve the slabs must keep free for the ones above to be given back, are asked of the slabs below too.
 */
extern uint32 sift_fsm_meta_short_below(Relation rel, BlockNumber from, bool for_move);


/* The first block of the metadata slab (not the first one) that ends at block end, InvalidBlockNumber when none does */
extern BlockNumber sift_fsm_slab_start_ending_at(Relation rel, BlockNumber end);

/* The pages in use of the metadata slabs that start at or above from */
extern uint32 sift_fsm_meta_used_from(Relation rel, BlockNumber from);

/* The free space under from becomes slabs as far as the slabs below it lack room for the pages of the ones at or above it to be moved down and for those to leave the file; false when the slabs below, with all the space that was there, still do not hold the pages in use above */
extern bool sift_fsm_slab_room_below(Relation rel, BlockNumber from);

/* The pages of the lists of the allocator (the free extents of each class and the queue) that lie at or above from are written to the lowest free pages of the slabs, when those are lower; the pages moved */
extern uint32 sift_fsm_rehome_lists(Relation rel, BlockNumber from);

#endif							/* SIFT_FSM_H */
