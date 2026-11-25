/*
 * core/f2/col_delta_anchor.h - where a partition's chain of per-column update pages (SIFT_PT_COL_DELTA, table/f2/col_delta/col_delta.h) starts.
 *
 * The anchor sits in the first SIFT_PT_PARTLIST page of the partition, right after parts[nparts], in the bytes that a full page leaves unused (a page holds at most
 * SIFT_PARTLIST_CAP parts and the 32 bytes of the anchor fit behind the last of them for every size of SiftPartDesc). SIFT_PARTLIST_FLAG_COL_DELTA says it is there and
 * SIFT_PARTENTRY_HAS_COL_DELTA says so in the partition entry, so a partition without a chain costs a reader one bit test and no page.
 */
#ifndef SIFT_COL_DELTA_ANCHOR_H
#define SIFT_COL_DELTA_ANCHOR_H

#include "core/host.h"

#define SIFT_PARTENTRY_HAS_COL_DELTA	(UINT32_C(1) << 5)
#define SIFT_PARTLIST_FLAG_COL_DELTA	(UINT16_C(1) << 0)

#define SIFT_COL_DELTA_ANCHOR_MAGIC		0x43444C41U		/* "CDLA" */
#define SIFT_COL_DELTA_ANCHOR_VER		1

#define SIFT_CDLA_DIRTY_TIME0			(UINT16_C(1) << 0)	/* the chain patches time_cols[0]: the time bounds of the partition's parts do not hold for it */
#define SIFT_CDLA_DIRTY_TIME1			(UINT16_C(1) << 1)
#define SIFT_CDLA_TID_ONLY				(UINT16_C(1) << 2)	/* the chain holds address changes only (SIFT_CD_ATT_TID_REMAP): no value of the index is patched, its summaries stay valid */
#define SIFT_CDLA_DIRTY_SCOPE			(UINT16_C(1) << 3)	/* the chain patches a leaf a scope summary of the index reads */
#define SIFT_CDLA_INDEXED_DIR			(UINT16_C(1) << 4)	/* delta_head_blkno points to SIFT_PT_COL_DELTA_DIR */
#define SIFT_CDLA_KNOWN					(SIFT_CDLA_DIRTY_TIME0 | SIFT_CDLA_DIRTY_TIME1 | SIFT_CDLA_TID_ONLY | SIFT_CDLA_DIRTY_SCOPE | SIFT_CDLA_INDEXED_DIR)

typedef struct SiftColDeltaAnchor
{
	uint32		magic;
	uint16		version;
	uint16		npages;			/* pages of the chain; 0 with no chain */
	BlockNumber delta_head_blkno;	/* the newest page of the chain, InvalidBlockNumber with no chain */
	uint16		flags;			/* SIFT_CDLA_* */
	uint16		pad;
	FullTransactionId min_xid;	/* the oldest writer of the chain when its first page was made (the pages' own min_xid is exact) */
	FullTransactionId max_xid;	/* the newest writer as of the last commit of the manifest that changed the chain */
} SiftColDeltaAnchor;

StaticAssertDecl(sizeof(SiftColDeltaAnchor) == 32, "SiftColDeltaAnchor must be 32 bytes");
StaticAssertDecl(offsetof(SiftColDeltaAnchor, min_xid) == 16, "SiftColDeltaAnchor.min_xid must sit at byte 16");

static inline SiftColDeltaAnchor
sift_col_delta_anchor_none(void)
{
	return (SiftColDeltaAnchor) {
		.magic = SIFT_COL_DELTA_ANCHOR_MAGIC,.version = SIFT_COL_DELTA_ANCHOR_VER,.delta_head_blkno = InvalidBlockNumber,
	};
}

static inline bool
sift_col_delta_anchor_has_chain(const SiftColDeltaAnchor *a)
{
	return a->delta_head_blkno != InvalidBlockNumber;
}

/* The anchor as a page holds it is what a build of this format wrote: its magic, its version and a chain that is either whole or absent */
static inline bool
sift_col_delta_anchor_sane(const SiftColDeltaAnchor *a)
{
	return a->magic == SIFT_COL_DELTA_ANCHOR_MAGIC && a->version == SIFT_COL_DELTA_ANCHOR_VER && a->pad == 0 &&
		(a->flags & ~SIFT_CDLA_KNOWN) == 0 &&
		(sift_col_delta_anchor_has_chain(a) ? a->npages >= 1 : a->npages == 0);
}

#endif							/* SIFT_COL_DELTA_ANCHOR_H */
