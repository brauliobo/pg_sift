/*
 * core/tidbridge.h - the TIDs a structured scan and a text scan hand each other, as page masks and Roaring-style offset containers per superblock.
 *
 * The producer adds heap keys (block << 16 | offset) in any order and finalizes the bridge once; the consumer then probes it. A superblock keeps the 256-bit
 * mask of its pages that hold a TID (page_mask) and of those it only knows as a whole (lossy_mask), so a ranked scan ANDs page_mask into the pages of a segment
 * superblock before it reads the docmap. The offsets of an exact page are an ARRAY of up to SIFT_TIDP_ARRAY_MAX sorted offsets or a BITMAP of SIFT_OFFMASK_WORDS words.
 *
 * The bridge never keeps more than its byte budget for pages and payload (the superblock headers aside): a page that would need more is LOSSY, which accepts every
 * offset of the page and so is a superset, never a miss. A TID is never lost: probes answer NO only for a TID that was not added.
 *
 * Optionally every exact TID carries a score (SIFT_TBR_HAS_SCORES) and a row locator (SIFT_TBR_HAS_ROWLOC), kept beside the containers in the order of the TIDs.
 * Portable (core/host.h).
 */
#ifndef SIFT_TIDBRIDGE_H
#define SIFT_TIDBRIDGE_H

#include "core/sift.h"

#define SIFT_TIDB_POINT_MAX		256		/* at most this many TIDs: the text scan reads them directly and skips the bounds */
#define SIFT_TIDP_ARRAY_MAX		16		/* a page of more offsets is a bitmap */
#define SIFT_TIDB_MAX_OFFSET	(SIFT_OFFMASK_WORDS * 64)

typedef enum SiftTidBridgeFlags
{
	SIFT_TBR_EXACT_QUALS = 1u << 0,		/* the producer evaluated every structured qual exactly */
	SIFT_TBR_EXACT_VIS = 1u << 1,		/* the producer evaluated the snapshot visibility exactly */
	SIFT_TBR_HAS_LOSSY = 1u << 2,		/* a page was kept lossy under the byte budget (set by the bridge) */
	SIFT_TBR_HAS_SCORES = 1u << 3,		/* a float4 score is kept for every exact TID */
	SIFT_TBR_HAS_ROWLOC = 1u << 4,		/* a SiftTidRowLoc is kept for every exact TID */
	SIFT_TBR_HAS_HOT_CHAINS = 1u << 5	/* the heap may hold HOT chains */
} SiftTidBridgeFlags;

typedef enum SiftTidPageKind
{
	SIFT_TIDP_ARRAY = 0,				/* sorted uint16 offsets, 1..SIFT_TIDP_ARRAY_MAX of them */
	SIFT_TIDP_BITMAP = 1,				/* SIFT_OFFMASK_WORDS words */
	SIFT_TIDP_LOSSY = 2					/* every offset of the page: no payload, the consumer re-checks */
} SiftTidPageKind;

typedef enum SiftTidProbe
{
	SIFT_TIDPROBE_NO = 0,
	SIFT_TIDPROBE_EXACT = 1,
	SIFT_TIDPROBE_LOSSY = 2				/* the page is kept whole: the TID may or may not have been added */
} SiftTidProbe;

/* One active superblock: 24 + 16 * SIFT_PAGEMASK_WORDS bytes, 88 with 256 pages */
typedef struct SiftTidBridgeSB
{
	uint32		sb;								/* block >> SIFT_SB_SHIFT */
	uint32		first_page_idx;					/* the first of its exact pages in SiftTidBridge.pages */
	uint64		page_mask[SIFT_PAGEMASK_WORDS];	/* pages of the superblock that hold a TID, exact or lossy */
	uint64		lossy_mask[SIFT_PAGEMASK_WORDS];	/* the pages kept whole */
	uint64		sb_blk_bloom;					/* OR of sift_tid_bloom_bit over its pages */
	uint32		active_pages;					/* popcount(page_mask) */
	uint32		pad;
} SiftTidBridgeSB;
StaticAssertDecl(sizeof(SiftTidBridgeSB) == 24 + 16 * SIFT_PAGEMASK_WORDS, "SiftTidBridgeSB: two page masks and four words");
#if SIFT_SB_SHIFT == 8
StaticAssertDecl(sizeof(SiftTidBridgeSB) == 88, "SiftTidBridgeSB must be 88 bytes");
#endif

/* The row of a TID in the columnar store: 8 bytes */
typedef struct SiftTidRowLoc
{
	uint32		local_g;			/* granule within the partition */
	uint16		part_slot;			/* partition of the open snapshot */
	uint16		row_ord;			/* row within the granule */
} SiftTidRowLoc;
StaticAssertDecl(sizeof(SiftTidRowLoc) == 8, "SiftTidRowLoc must be 8 bytes");

/* An exact or lossy-by-promotion page: lossy pages of a finished bridge have no record (their bit in lossy_mask is all there is) */
typedef struct SiftTidBridgePage
{
	uint8		kind;				/* SiftTidPageKind */
	uint8		n;					/* offsets of an ARRAY page */
	uint16		page_in_sb;
	uint32		payload_off;		/* first uint16 in array_pool, or row in bm_pool */
	uint32		side_off;			/* index of its first TID in scores and rowlocs */
} SiftTidBridgePage;
StaticAssertDecl(sizeof(SiftTidBridgePage) == 12, "SiftTidBridgePage must be 12 bytes");

/* A score or row locator that waits for finalize to find its place among the sorted TIDs of its page */
typedef struct SiftTidBridgePending
{
	uint64		key;
	SiftTidRowLoc loc;
	float4		score;
	uint32		seq;
	uint32		has;				/* SIFT_TBR_HAS_SCORES and SIFT_TBR_HAS_ROWLOC bits this entry sets */
	uint32		pad;
} SiftTidBridgePending;

/* uint32 -> uint32 open-addressing map of the build: superblock to its entry, block to its page record */
typedef struct SiftTidBridgeMap
{
	uint32	   *key;
	uint32	   *val;
	uint32		mask;				/* slots - 1, 0 without slots */
	uint32		n;
} SiftTidBridgeMap;

typedef struct SiftTidBridge
{
	MemoryContext mcxt;
	uint32		flags;				/* SiftTidBridgeFlags */
	Size		mem_budget;
	bool		finalized;

	SiftTidBridgeSB *sbs;			/* ascending sb after finalize */
	uint32		nsbs;
	uint32		sb_cap;

	SiftTidBridgePage *pages;		/* by (sb, page_in_sb) after finalize */
	uint32		npages;
	uint32		page_cap;

	uint16	   *array_pool;			/* the offsets of the ARRAY pages: SIFT_TIDP_ARRAY_MAX slots each while building, packed after finalize */
	uint32		array_len;
	uint32		array_cap;

	uint64	  (*bm_pool)[SIFT_OFFMASK_WORDS];
	uint32		bm_len;
	uint32		bm_cap;

	float4	   *scores;				/* ntids entries after finalize, in the order of the TIDs of the superblocks */
	SiftTidRowLoc *rowlocs;

	uint64		ntids;				/* the TIDs of the exact pages: a lower bound of the TIDs added once a page is lossy */
	uint64		min_tid;			/* the smallest and largest key a TID may have (a lossy page counts whole); UINT64_MAX and 0 when empty */
	uint64		max_tid;
	uint64		blk_bloom_union;	/* OR of the sb_blk_bloom of every superblock */

	uint32	   *pblk;				/* the build: block of every page record */
	SiftTidBridgePending *pending;
	uint64		npending;
	uint64		pending_cap;
	SiftTidBridgeMap sb_map;
	SiftTidBridgeMap page_map;
	uint32		hot_sb;				/* the superblock entry and the page record of the last add */
	uint32		hot_page;
	uint32		hot_blk;
} SiftTidBridge;

/* The bloom bit of a block in sb_blk_bloom: one hash for every writer and reader of the bloom */
static inline uint64
sift_tid_bloom_bit(BlockNumber blk)
{
	return UINT64CONST(1) << (((uint64) blk * UINT64CONST(0x9E3779B97F4A7C15)) >> 58);
}

/* flags: the SIFT_TBR_EXACT_*, HAS_HOT_CHAINS facts of the producer and the SIFT_TBR_HAS_SCORES / HAS_ROWLOC the bridge keeps; mem_budget_bytes caps pages and payload */
extern SiftTidBridge *sift_tidbridge_create(MemoryContext mcxt, Size mem_budget_bytes, uint32 flags);

/* Empties the bridge for the next chunk; the allocations are kept */
extern void sift_tidbridge_reset(SiftTidBridge *br);

/* Adds a TID (a duplicate changes nothing); keys in any order; the offset is 1..SIFT_TIDB_MAX_OFFSET */
extern void sift_tidbridge_add_tid(SiftTidBridge *br, SiftKey key);

/* Adds a TID and its score (the last score of a key wins); the bridge has SIFT_TBR_HAS_SCORES. The score of a page that goes lossy is dropped */
extern void sift_tidbridge_add_score(SiftTidBridge *br, SiftKey key, float4 score);

/* Adds a TID and its row locator; the bridge has SIFT_TBR_HAS_ROWLOC */
extern void sift_tidbridge_add_rowloc(SiftTidBridge *br, SiftKey key, SiftTidRowLoc loc);

/* Sorts the superblocks and pages, packs the payload, builds the blooms and the key range, places the scores and row locators; no add after it */
extern void sift_tidbridge_finalize(SiftTidBridge *br);

/* The bytes the byte budget counts: page records, payload, pending scores and row locators and the map of the pages (the superblock entries are outside it) */
extern Size sift_tidbridge_bytes(const SiftTidBridge *br);

/* The entry of superblock sb, NULL when it holds no TID */
extern const SiftTidBridgeSB *sift_tidbridge_probe_sb(const SiftTidBridge *br, uint32 sb);

/* Is the TID in the bridge; *score (when not NULL and the bridge has scores) is its score when the answer is EXACT, 0 for a TID added without one */
extern SiftTidProbe sift_tidbridge_probe_tid(const SiftTidBridge *br, SiftKey key, float4 *score);

/* The row locator of an exact TID; false for any other */
extern bool sift_tidbridge_rowloc(const SiftTidBridge *br, SiftKey key, SiftTidRowLoc *out);

/*
 * The offset masks of the SIFT_BLK_PAGES pages of scoring block b of superblock bsb, written for the pages the bridge holds only (the rows of the others are not touched);
 * the lossy pages are all ones. Returns the mask of those pages (bit i: page i of the block); *out_lossy_pages (when not NULL) the mask of the lossy ones among them.
 */
extern uint64 sift_tidbridge_block_mask(const SiftTidBridge *br, const SiftTidBridgeSB *bsb, int b, uint64 (*out)[SIFT_OFFMASK_WORDS],
										uint64 *out_lossy_pages);

/* out = pages & the pages of bsb (out may be pages); is any left: a segment superblock with none has no TID of the bridge, whatever its docmap says */
static inline bool
sift_tidbridge_intersect_pages(const SiftTidBridgeSB *bsb, const uint64 *pages, uint64 *out)
{
	uint64		any = 0;

	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		any |= out[w] = pages[w] & bsb->page_mask[w];
	return any != 0;
}

/* Few enough exact TIDs to read them one by one instead of ranking the whole segment */
static inline bool
sift_tidbridge_is_point(const SiftTidBridge *br)
{
	return !(br->flags & SIFT_TBR_HAS_LOSSY) && br->ntids <= SIFT_TIDB_POINT_MAX;
}

/*
 * The finalized bridge of the documents (any order) of a host whose docid d is the key (d / dpp) << 16 | (d % dpp + 1): no byte budget, so no lossy page
 * (the MariaDB host's filter of a ranked scan)
 */
extern SiftTidBridge *sift_tidbridge_from_docids(MemoryContext mcxt, const uint64 *docids, int64 n, uint32 dpp);

/* Is a row range (granule: tid_min..tid_max, a bit per block in blk_bloom) possibly in the bridge: the key range, the bloom of the blocks and the pages of its superblocks */
extern bool sift_tidbridge_granule_may_match(const SiftTidBridge *br, uint64 tid_min, uint64 tid_max, uint64 blk_bloom);

/* live &= the rows in the bridge (an exact bridge); score_out[r] their scores; returns the rows left. A lossy page is an error: a text scan's bridge never has one */
extern int	sift_tidbridge_probe_vec(const SiftTidBridge *br, const uint64 *tid, const uint64 *live, int n, uint64 *live_out, float4 *score_out);

#endif							/* SIFT_TIDBRIDGE_H */
