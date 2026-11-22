/*
 * core/meta.h - format major 2: the prefix every metapage starts with, its checksum and the one check that opens an index.
 *
 * Block 0 begins with SiftMetaCommon, then the payload of the access method (col/col_meta.h). The major version sits at the byte
 * offset of the metapage of major 1, so that the opener tells an index of the older format apart before it looks at anything that
 * moved: such an index is a version mismatch with a REINDEX (or VACUUM FULL) hint, never damage and never quarantined.
 * Every metadata page of format 2 carries a CRC32C of its payload; the pages are immutable once published (core/manifest.h).
 */
#ifndef SIFT_META_H
#define SIFT_META_H

#include "core/host.h"

#include "core/page.h"
#include "core/part_lock.h"
#include "core/xlog.h"


#define SIFT_FSM_BLKNO			1

/* The page locks of format 2 name no block: they sit above the last block an index may have (SIFT_F2_LOCK_FLOOR), below it are the locks of format 1 (0..6) */
#define SIFT_F2_LOCK_FLOOR			SIFT_PART_LOCK_BASE

typedef enum SiftAmKind
{
	SIFT_AM_BM25 = 1,
	SIFT_AM_COL = 2,
	SIFT_AM_TABLE = 3,
	SIFT_AM_VSTORE = 4
} SiftAmKind;

/* Required to read: a bit this build does not know refuses the open */
#define SIFT_COMPAT_PARTITIONED		(UINT64CONST(1) << 0)
#define SIFT_COMPAT_NATIVE_MVCC		(UINT64CONST(1) << 1)
#define SIFT_COMPAT_TRACK_DELETES	(UINT64CONST(1) << 2)
#define SIFT_COMPAT_SCOPES			(UINT64CONST(1) << 3)
#define SIFT_COMPAT_TIME_BOUNDS		(UINT64CONST(1) << 4)	/* the 192-byte partition entries, the 224-byte part descriptors and the 64-byte directory keys carry two time bounds */
#define SIFT_COMPAT_REMOTE_PARTS	(UINT64CONST(1) << 5)	/* a reader must know how to fetch the parts that live in remote storage (set only while the relation has one) */
#define SIFT_KNOWN_COMPAT_MASK		(SIFT_COMPAT_PARTITIONED | SIFT_COMPAT_NATIVE_MVCC | SIFT_COMPAT_TRACK_DELETES | SIFT_COMPAT_SCOPES | SIFT_COMPAT_TIME_BOUNDS | SIFT_COMPAT_REMOTE_PARTS)

/* Required to write: a bit this build does not know leaves the index readable and refuses a write or maintenance step */
#define SIFT_ROCOMPAT_PREAGG_V2		(UINT64CONST(1) << 0)
/* bit 1 is retired for good (the group ordinals of format 1) */
#define SIFT_ROCOMPAT_META_INLINE	(UINT64CONST(1) << 2)	/* block 0 holds the inline summary after the metapage struct (col/col_meta.h) */
#define SIFT_ROCOMPAT_PROJECTIONS	(UINT64CONST(1) << 3)	/* writers and compaction must maintain the secondary projections (set only while the relation has one) */
#define SIFT_ROCOMPAT_TTL			(UINT64CONST(1) << 4)	/* writers and compaction must enforce the time to live (set only while the relation has one) */
#define SIFT_KNOWN_ROCOMPAT_MASK	(SIFT_ROCOMPAT_PREAGG_V2 | SIFT_ROCOMPAT_META_INLINE | SIFT_ROCOMPAT_PROJECTIONS | SIFT_ROCOMPAT_TTL)

/* SiftColMetaPageData.state_flags: states, not format */
#define SIFT_META_FLAG_QUARANTINED	(UINT32_C(1) << 0)
#define SIFT_META_FLAG_UNKEYED		(UINT32_C(1) << 1)
#define SIFT_META_FLAG_AUTO_PENDING	(UINT32_C(1) << 2)	/* format 2: built before the rows chose its jsonb paths: those leaves are not there and a qual on them is the heap's */

typedef struct SiftMetaCommon
{
	uint32		magic;
	uint16		format_major;
	uint16		format_minor;
	uint16		am_kind;		/* SiftAmKind */
	uint16		struct_size;	/* the bytes of the metapage struct that follows the page header */
	uint32		crc32c;			/* over [offsetof(compat_flags), struct_size) */
	uint64		compat_flags;
	uint64		ro_compat_flags;
} SiftMetaCommon;

StaticAssertDecl(sizeof(SiftMetaCommon) == 32, "SiftMetaCommon must be 32 bytes");
StaticAssertDecl(offsetof(SiftMetaCommon, format_major) == 4, "format_major stays where the metapage of major 1 has it");

#define SIFT_META_CRC_FROM		offsetof(SiftMetaCommon, compat_flags)

extern uint32 sift_crc32c(const void *data, Size len);

/* The page as format 2 lays it out: type, generation and partition in the special space, nothing else set */
extern void sift_page_init_part(Page page, SiftPageType type, uint64 gen, uint32 part_id);

/* ERROR (SIFT_CORRUPT) unless the page is laid out as a page of the index and carries exactly this type, generation and partition */
/* Any partition: a part keeps the number of the partition it was written for when a merge of partitions moves it to the other, and the generation says which part it is */
#define SIFT_PART_ID_ANY	UINT32_MAX

extern void sift_page_expect(Relation rel, Page page, BlockNumber blk, SiftPageType type, uint64 gen, uint32 part_id);

/* The same for a reader that holds the manifest of version snap_manifest_ver: on a standby a mismatch is a serialization failure (retry) once the manifest has moved on, see sift_page_expect */
extern void sift_page_expect_snap(Relation rel, Page page, BlockNumber blk, SiftPageType type, uint64 gen, uint32 part_id, uint64 snap_manifest_ver);

/*
 * Every other page of format 2 starts with a 32-bit magic, two 16-bit words and then its CRC32C (at SIFT_PAGE_CRC_OFF): the sum is of the payload bytes from after the field
 * to pd_lower. Sealed when the page is built or changed, checked when it is read.
 */
#define SIFT_PAGE_CRC_OFF		8
#define SIFT_PAGE_CRC_FROM		12
extern void sift_page_seal(Page page);
extern bool sift_page_sum_ok(const Page page);

/* The block of a relation read or overwritten for a record of generic WAL: an exclusively locked buffer; sift_blk_new extends the relation to the block when it is past its end and zeroes it */
extern Buffer sift_blk_read(Relation rel, BlockNumber blk, int lockmode);
extern Buffer sift_blk_new(Relation rel, BlockNumber blk);

/*
 * The one open of an index: checks the page is block 0 of format 2 and returns its metapage (in the page). The order is fixed: magic, then the major
 * version (ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE with a REINDEX or VACUUM FULL hint for any other major), then the sizes, the checksum and the feature bits.
 * for_write also refuses a read-only compatible bit this build does not know.
 */
extern const SiftMetaCommon *sift_store_open_meta(Relation rel, Page page, SiftAmKind am, Size min_size, bool for_write);

/* Logs the record of x and unlocks and releases every buffer it registered */
extern void sift_xlog_done(SiftXlog *x);

/* The new pages of one write (a chain of list pages), each a full image, logged up to MAX_GENERIC_XLOG_PAGES to a record: nothing names them before the change that publishes them */
typedef struct SiftNewPageLog
{
	Relation	index;
	SiftXlog	x;
} SiftNewPageLog;

#define SIFT_NEWLOG_BEGIN(rel)	((SiftNewPageLog) {.index = (rel)})

/* The working copy of the new page blk, to build; the record it is in is logged when it is full or at sift_newlog_end */
extern Page sift_newlog_page(SiftNewPageLog *l, BlockNumber blk);
extern void sift_newlog_end(SiftNewPageLog *l);

/* Sets struct_size, pd_lower and the checksum of a metapage whose struct was changed in the working copy of a record */
extern void sift_meta_seal(Page page, Size struct_size);

#endif							/* SIFT_META_H */
