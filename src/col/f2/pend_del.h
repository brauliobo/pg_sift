/*
 * col/pend_del.h - the pending deletes of a partition (format major 2): rows deleted one at a time are appended as (tid, xmax, cmax) to a chain of pages that hangs from the
 * partition's entry, under the partition's lock, without a read of any part. Readers check the chain (a few pages) for the TIDs they meet; a fold (col/delete.h) resolves the
 * TIDs into the deletion vectors of the parts and drops the chain.
 */
#ifndef SIFT_PEND_DEL_H
#define SIFT_PEND_DEL_H

#include "core/host.h"

#include "col/granule/granule.h"
#include "core/f2/manifest.h"
#include "core/xlog.h"

#define SIFT_PEND_DEL_MAGIC		0x5044454CU		/* "PDEL" */
#define SIFT_PEND_DEL_SEALED	(UINT16_C(1) << 0)
#define SIFT_PEND_DEL_SHARD_DIR	(UINT16_C(1) << 1)	/* the page is a shard directory's (col/f2/l0_shard_dir.h) */
#define SIFT_PEND_DEL_FOLD_PAGES 4

typedef struct SiftPendDelRec
{
	uint64		tid;			/* (block << 16) | offset */
	TransactionId xmax;
	CommandId	cmax;
} SiftPendDelRec;

StaticAssertDecl(sizeof(SiftPendDelRec) == 16, "SiftPendDelRec must be 16 bytes");

/*
 * An address uses 48 bits. The delete of the version an UPDATE replaced is two adjacent records, never cut by a page: {old | SIFT_PD_TID_UPDATED, xmax, cmax} and the successor
 * {new | SIFT_PD_TID_SUCC, InvalidTransactionId, 0}. A transaction that waited for the row finds where it went there when the lock table has lost it; a fold into the deletion
 * vectors (sift_pend_del_unpair) keeps the pair until the deleter is past every horizon.
 */
#define SIFT_PD_TID_MASK		((UINT64CONST(1) << 48) - 1)
#define SIFT_PD_TID_UPDATED		(UINT64CONST(1) << 63)
#define SIFT_PD_TID_SUCC		(UINT64CONST(1) << 62)

typedef struct SiftPendDelPageData
{
	uint32		magic;
	uint16		flags;
	uint16		nrecs;
	uint32		crc32c;
	union
	{
		BlockNumber next_blk;	/* a forward chain (flags bits 8..15 are 0): the newer page */
		BlockNumber prev_blk;	/* a reverse chain: the older page; flags bits 8..15 are the pages of the chain */
	};
	FullTransactionId min_xid;
	FullTransactionId max_xid;
	SiftPendDelRec recs[FLEXIBLE_ARRAY_MEMBER];
} SiftPendDelPageData;

StaticAssertDecl(offsetof(SiftPendDelPageData, recs) == 32, "SiftPendDelPageData header is 32 bytes");

#define SIFT_PEND_DEL_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftPendDelPageData, recs)) / sizeof(SiftPendDelRec))

/* Appends the records to the chain of the partition (created on the first); returns the pages the chain has now */
extern int	sift_pend_del_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n);

/* The records as the last of the tail page of the chain at head, registered in x with its buffer locked; false, with nothing registered, when they do not fit it or the chain has the pages that fold it */
/* A first page of pending deletes holding recs (at most SIFT_PEND_DEL_CAP), registered in x: its block, for the manifest entry of the partition */
extern BlockNumber sift_pend_del_first(SiftXlog *x, Relation rel, uint64 part_id, const SiftPendDelRec *recs, int n);
extern bool sift_pend_del_place(SiftXlog *x, Relation rel, uint64 part_id, BlockNumber head, const SiftPendDelRec *recs, int n);

/*
 * The records as plain deletes, in place: the successors go and the flags of the others (the number left). The pairs whose deleter some snapshot may still wait for are copied to keep
 * (nkeep records, room for n), to be appended again to the chain the fold leaves.
 */
extern int	sift_pend_del_unpair(SiftPendDelRec *recs, int n, SiftPendDelRec *keep, int *nkeep);

/* The stamp of the chain at head: one page header read for each page, no record copied */
extern SiftChainStamp sift_pend_del_stamp(Relation rel, uint64 part_id, BlockNumber head);

/* The records of the chain at head, in order, and the blocks of its pages */
extern SiftPendDelRec *sift_pend_del_read(Relation rel, uint64 part_id, BlockNumber head, int *n, BlockNumber **pages, int *npages);

/*
 * The addresses a reader met in the pending deletes of a partition and the snapshot sees deleted, as a set: the range and the blocks of the addresses (the blocks as the bloom of a
 * granule's entry in SEC_TID_INDEX does) tell most granules in three comparisons that none of their rows is in it, and the rest ask the hash, not a binary search for each row.
 */
typedef struct SiftPendDelIndex
{
	uint64		tid_min;
	uint64		tid_max;
	uint64		blk_bloom;
	uint32		n;
	uint32		mask;			/* the slots of htab less one: a power of two, at least twice n */
	uint64	   *htab;			/* an address in the slot of its hash, 0 for a free one (an address has an offset, so is not 0) */
} SiftPendDelIndex;

/* The set of the n addresses (n > 0, in any order) */
extern SiftPendDelIndex *sift_pdel_idx_build(const uint64 *tids, int n);

/* The set and its table; NULL is no set */
extern void sift_pdel_idx_free(SiftPendDelIndex *px);

static inline bool
sift_pdel_idx_may_hit_granule(const SiftPendDelIndex *px, const SiftTidIndexEntry *ix)
{
	return ix->tid_max >= px->tid_min && ix->tid_min <= px->tid_max && (ix->blk_bloom & px->blk_bloom) != 0;
}

static inline bool
sift_pdel_idx_has(const SiftPendDelIndex *px, uint64 tid)
{
	for (uint32 slot = (uint32) ((tid * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & px->mask;; slot = (slot + 1) & px->mask)
	{
		if (px->htab[slot] == tid)
			return true;
		if (px->htab[slot] == 0)
			return false;
	}
}

#endif							/* SIFT_PEND_DEL_H */
