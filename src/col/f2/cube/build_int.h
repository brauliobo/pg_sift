/*
 * col/f2/cube/build_int.h - what build.c (the granules, the nodes) and evict.c (the budget: evict open nodes, spill month cubes) of the summary
 * build share: the accumulators of a key and the state of one build.
 */
#ifndef SIFT_COL_SCOPE_V6_BUILD_INT_H
#define SIFT_COL_SCOPE_V6_BUILD_INT_H

#include "col/f2/cube/int.h"
#include "col/f2/cube/wire.h"

typedef struct KeyAcc
{
	uint32		ord;			/* ordinal in the pair map */
	int64		scope_key;
	uint32		narrivals;		/* refs appended so far: the arrival ordinal of the next one */
	SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
	uint16		nrefs;
	bool		impure;
	bool		members_unknown;	/* a clustered granule: its members are not listed, so the node keeps a range only */
	int64		mlo,
				mhi;
	uint32		mcount;
	int64		last_member;
	int64	   *members;		/* coded members of the open node */
	uint32		nmembers;
	uint32		capmembers;
	uint32		first_arrival;
	SiftScopeCubeAcc l1_all;
	SiftScopeMonthTable l1_months;
	SiftScopeCubeAcc l2_all;
	SiftScopeMonthTable l2_months;
	uint64		member_total;
	SiftTopKHeap *ring;			/* the rings of the key: ring 0 of the time leaf, then the extra order leaves */
	SiftMonthMapAcc mmap;
	Size		est_base;		/* the bytes the accumulators of the key hold, in three parts: what stays, the open node, the month cubes of level 2 */
	Size		est_open;
	Size		est_l2m;
	struct KeyAcc *lru_prev;	/* the keys with month cubes of level 2 in memory, most recently touched first */
	struct KeyAcc *lru_next;
	bool		in_lru;
	struct KeySpill *spill;		/* the month cubes of level 2 that went out of memory, the latest spill first */
} KeyAcc;

/* One spill: nmonths, (month, length) for each, the cubes */
typedef struct KeySpill
{
	struct KeySpill *next;
	char	   *blob;
} KeySpill;

typedef struct NodeRec
{
	SiftScopeL1Node d;			/* key_ordinal: the ordinal in the pair map until the end */
	uint32		pm_ord;
	uint32		block_len;
	uint32		first_arrival;
} NodeRec;

struct SiftScopeBuild
{
	MemoryContext mcxt;
	MemoryContext caller;		/* the context the sources allocate in: what they return lives as long as the caller says */
	SiftScopeBuildSpec spec;
	SiftScopePairMap *pm;
	KeyAcc	  **acc;
	uint32		nactive;
	Size		live;
	Size		budget;
	Size	   *open_size;		/* the bytes of the open node of each key: what a flush of it gives back */
	SiftMaxHeap open;			/* the keys with an open node, the largest on top */
	KeyAcc	   *lru_head;
	KeyAcc	   *lru_tail;
	const SiftScopeGranMask *mask;
	StringInfoData l0pool;
	SiftScopeL0SliceEntry *slices;
	uint32		nslices;
	uint32		capslices;
	StringInfoData l1pool;
	NodeRec    *nodes;
	uint32		nnodes;
	uint32		capnodes;
	SiftScopePartMapEntry *parts;
	uint32		nparts;
	uint32		cap_parts;
	uint32		total_granules;
	SiftScopeDict dict;
	uint32	   *slot_of_key;	/* per granule scratch: slot of a key in this granule, UINT32_MAX none */
	SiftScopeBuildStats stats;
	uint32		nrings;
	uint32		depth[SIFT_SCOPE_TOPK_RINGS_CAP];	/* the rows each ring keeps for a key */
	SiftHomeSet homes;			/* the entity runs of the granule in hand and the homes of every entity so far (a scope clustered by primary key) */
};

#define IN_BUILD(b)	MemoryContext old_cx_ = MemoryContextSwitchTo((b)->mcxt); (b)->caller = old_cx_
#define OUT_BUILD()	MemoryContextSwitchTo(old_cx_)

#define SRC_CALL(b, call) \
	do { \
		MemoryContext o_ = MemoryContextSwitchTo((b)->caller); \
		call; \
		MemoryContextSwitchTo(o_); \
	} while (0)

/* Writes the open node of a key to the level-1 pool and folds its cubes into the level-2 ones; release: the arrays of the node go back to the allocator */
extern void sift_sbld_flush_l1(SiftScopeBuild *b, KeyAcc *k, bool release);
/* Brings the bytes the build counts for a key, the heap of open nodes and the list of keys with month cubes in memory up to what the key holds now */
extern void sift_sbld_account(SiftScopeBuild *b, KeyAcc *k);
extern void sift_sbld_lru_front(SiftScopeBuild *b, KeyAcc *k);
/* The month cubes of level 2 of a key that went out of memory come back into its table */
extern void sift_sbld_unspill(SiftScopeBuild *b, KeyAcc *k);
/* Brings the accumulators back under the budget: flushes open nodes, then spills month cubes */
extern void sift_sbld_evict_if_over(SiftScopeBuild *b);

#endif							/* SIFT_COL_SCOPE_V6_BUILD_INT_H */
