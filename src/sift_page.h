/*
 * sift_page.h - on-disk format. Changing anything here
 * requires a SIFT_FORMAT_MAJOR or _MINOR bump.
 *
 * Every page: standard PageHeader + SiftPageOpaqueData special space, and
 * pd_upper == pd_special. Content lives in [SIFT_PAGE_DATA_OFFSET, pd_lower):
 * generic WAL and full-page images treat [pd_lower, pd_upper) as a hole and
 * replay it as zeros, so pd_lower must always cover every used byte.
 * Data pages carry a raw byte stream of SIFT_PAGE_PAYLOAD bytes.
 */
#ifndef SIFT_PAGE_H
#define SIFT_PAGE_H

#include "sift_host.h"

#include "sift.h"

#define SIFT_METAPAGE_BLKNO		0
#define SIFT_DIRECTORY_BLKNO	1
#define SIFT_MAINT_LOCK_BLKNO	0	/* LockPage() target for maintenance */
#define SIFT_ACTIVE_LOCK_BLKNO	2	/* LockPage() target serializing active segment changes */

typedef enum SiftPageType
{
	SIFT_PT_META = 1,
	SIFT_PT_DIRECTORY,
	SIFT_PT_SEGHEADER,			/* immutable segment header */
	SIFT_PT_DATA,				/* immutable segment byte stream */
	SIFT_PT_LIVE,				/* liveness bits / dirty-SB bitmap */
	SIFT_PT_MUTHEADER,			/* mutable segment lane headers */
	SIFT_PT_MUTDATA,			/* mutable doc records */
	SIFT_PT_FREELIST,			/* free extent records */
	SIFT_PT_FREE				/* unused, reclaimable */
} SiftPageType;

typedef struct SiftPageOpaqueData
{
	uint16		type;			/* SiftPageType */
	uint16		flags;
	uint32		reserved;
	uint64		gen;			/* owning segment generation; 0 for meta/dir */
} SiftPageOpaqueData;

typedef SiftPageOpaqueData *SiftPageOpaque;

#define SiftPageGetOpaque(page) ((SiftPageOpaque) PageGetSpecialPointer(page))
#define SIFT_PAGE_DATA_OFFSET	MAXALIGN(SizeOfPageHeaderData)
#define SIFT_PAGE_PAYLOAD \
	(BLCKSZ - SIFT_PAGE_DATA_OFFSET - MAXALIGN(sizeof(SiftPageOpaqueData)))
#define SiftPageGetData(page)	((char *) (page) + SIFT_PAGE_DATA_OFFSET)
#define SiftPageGetUsed(page)	((uint32) (((PageHeader) (page))->pd_lower - SIFT_PAGE_DATA_OFFSET))
#define SiftPageSetUsed(page, n) (((PageHeader) (page))->pd_lower = SIFT_PAGE_DATA_OFFSET + (n))

/* Metapage (block 0) */
typedef struct SiftMetaPageData
{
	uint32		magic;
	uint16		format_major;
	uint16		format_minor;
	float4		k1;				/* options snapshot at build time */
	float4		b;
	uint32		analyzer_hash;
	uint32		flags;			/* bits 0-7: failed maintenance steps in a row (backoff); older indexes: 0 */
	uint64		next_gen;		/* next segment generation */
	uint64		ndocs;			/* totals over immutable segments */
	uint64		sum_dl;
	BlockNumber freelist_head;	/* SIFT_PT_FREELIST chain or InvalidBlockNumber */
	BlockNumber high_water;		/* blocks ever allocated */
	TimestampTz maint_requested_at; /* async maintenance dedup */
} SiftMetaPageData;

#define SIFT_META_FAILURES_MASK	0xFF

/* Segment directory (block 1) */
typedef enum SiftSegKind
{
	SIFT_SEG_IMMUTABLE = 1,
	SIFT_SEG_MUTABLE_ACTIVE,
	SIFT_SEG_MUTABLE_SEALED
} SiftSegKind;

#define SIFT_SEG_DIRTY	0x0001	/* some liveness bit cleared */
#define SIFT_SEG_POSITIONS 0x0002	/* written with word positions (SIFT_TERM_POS terms) */

typedef struct SiftSegEntry
{
	uint64		gen;
	uint8		kind;			/* SiftSegKind */
	uint8		format;			/* segment format minor */
	uint16		flags;
	BlockNumber header_blk;		/* SEGHEADER or MUTHEADER */
	uint32		sb_lo;			/* SB range covered (inclusive) */
	uint32		sb_hi;
	uint32		reserved;
	uint64		ndocs;
	uint64		sum_dl;
	uint64		bytes;
} SiftSegEntry;

typedef struct SiftDirectoryData
{
	uint32		version;		/* bumped by every directory update */
	uint16		nsegs;
	uint16		reserved;
	SiftSegEntry segs[FLEXIBLE_ARRAY_MEMBER];
} SiftDirectoryData;

StaticAssertDecl(offsetof(SiftDirectoryData, segs) + SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry)
				 <= SIFT_PAGE_PAYLOAD, "directory must fit one page");

/* Immutable segment header page */
typedef enum SiftSection
{
	SIFT_SEC_DOCMAP = 0,
	SIFT_SEC_POSTINGS,
	SIFT_SEC_DICT,
	SIFT_SEC_DICTIDX,
	SIFT_NSECTIONS
} SiftSection;

typedef struct SiftExtent
{
	BlockNumber start;
	uint32		nblocks;
} SiftExtent;

typedef struct SiftSectionRef
{
	uint64		off;			/* logical byte offset in the segment stream */
	uint64		len;
} SiftSectionRef;

typedef struct SiftSegHeaderData
{
	uint64		gen;
	uint64		ndocs;
	uint64		sum_dl;
	uint64		nterms;
	float4		avgdl_ref;		/* avgdl used for stored bounds */
	uint32		sb_lo;
	uint32		sb_hi;
	BlockNumber live_first;		/* first LIVE page (contiguous run) */
	uint32		live_npages;
	uint32		dictroot_n;		/* entries in the DICTIDX root array */
	uint64		dictroot_off;	/* logical offset of the root array */
	SiftSectionRef sections[SIFT_NSECTIONS];
	uint32		nextents;
	SiftExtent	extents[FLEXIBLE_ARRAY_MEMBER];	/* logical stream order */
} SiftSegHeaderData;

#define SIFT_MAX_EXTENTS \
	((SIFT_PAGE_PAYLOAD - offsetof(SiftSegHeaderData, extents)) / sizeof(SiftExtent))

/*
 * Liveness: the first LIVE page starts with SiftLiveHeaderData followed by
 * the dirty-SB bitmap (bit sb - sb_lo); liveness bits (doc rank order, 1 =
 * live) follow and continue over the next live_npages - 1 pages.
 */
typedef struct SiftLiveHeaderData
{
	uint32		nsb;			/* bits in the dirty-SB bitmap */
	uint32		ndead;			/* cleared liveness bits */
} SiftLiveHeaderData;

/*
 * Mutable segment: one MUTHEADER page with SIFT_LANES lanes. A lane always has
 * a head page; tail is a hint (the true tail is found by following next) kept
 * with the page count when a page is added. The page after the last one has
 * next = InvalidBlockNumber (the store walks these chains for the orphan
 * sweep and verification).
 */
typedef struct SiftLaneData
{
	BlockNumber head;
	BlockNumber tail;
	uint32		npages;
} SiftLaneData;

typedef struct SiftMutHeaderData
{
	uint64		gen;
	SiftLaneData lanes[SIFT_LANES];
} SiftMutHeaderData;

/*
 * Mutable data page: header, then doc records packed back to back. The
 * SEALED flag is set under the page lock on the tail of every lane when the
 * segment is sealed; appenders re-check it under that lock. SPANS marks a
 * page whose last doc continues on the next page of the lane.
 */
typedef struct SiftMutPageHeaderData
{
	BlockNumber next;
	uint16		ndocs;			/* docs started on this page (deleted included) */
	uint16		flags;
	uint32		sum_dl;
	uint64		bloom[4];		/* 256-bit bloom of term hashes, 2 probes */
} SiftMutPageHeaderData;

#define SIFT_MUTPAGE_SEALED	0x0001
#define SIFT_MUTPAGE_SPANS	0x0002

#define SIFT_MUT_DELETED	0x0001
#define SIFT_MUT_CONT		0x0002	/* more terms of the preceding doc: no dl, not counted */

/*
 * Doc record, 4-byte aligned:
 *   SiftMutDocData, uint32 hash[nterms], uint8 tf[nterms], uint8 len[nterms],
 *   term bytes (concatenated, sum(len) = termbytes), zero pad to 4 bytes.
 * A doc that does not fit a page is split into pieces: the first carries
 * dl, the others SIFT_MUT_CONT. All pieces of a doc are adjacent in one lane.
 */
typedef struct SiftMutDocData
{
	ItemPointerData tid;
	uint16		flags;
	uint16		nterms;
	uint16		dl;
	uint16		termbytes;
	uint16		reserved;
} SiftMutDocData;

/*
 * Free list: a chain of FREELIST pages (gen 0) from meta->freelist_head.
 * Records are reusable once safexid precedes the GlobalVis horizon; an
 * invalid safexid means reusable now. Records never overlap each other or
 * any referenced block, and lie in [SIFT_DIRECTORY_BLKNO + 1, high_water).
 */
typedef struct SiftFreeExtent
{
	BlockNumber start;
	uint32		nblocks;
	FullTransactionId safexid;
} SiftFreeExtent;

typedef struct SiftFreeListData
{
	BlockNumber next;
	uint32		n;
	SiftFreeExtent ext[FLEXIBLE_ARRAY_MEMBER];
} SiftFreeListData;

#define SIFT_FREELIST_CAP \
	((SIFT_PAGE_PAYLOAD - offsetof(SiftFreeListData, ext)) / sizeof(SiftFreeExtent))

#endif							/* SIFT_PAGE_H */
