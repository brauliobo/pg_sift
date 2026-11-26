/*
 * table/f2/blob.h - the values of a sift_table too wide for a granule (SIFT_TBL_WIDE_THRESHOLD bytes up to 1 GB) kept in extents of their own, or in their reference when they
 * compress to no more than SIFT_TBL_WIDE_THRESHOLD bytes (SIFT_BLOB_FLAG_INLINE: no page, no extent, the stored bytes follow the header).
 *
 * A wide value stays in memory with its row in the stage (tag SIFT_COLTAG_WIDE_RAW, the raw value as the payload of the cell) and reaches the extents only when the stage is
 * written, so a transaction that aborts before that leaves no page behind. The row then holds the reference (SiftTblBlobRef plus its extents) under SIFT_COLTAG_WIDE_BLOB.
 * A compaction copies the reference only; the extents of a row it drops go to the release list of its manifest edit.
 */
#ifndef SIFT_TBL_F2_BLOB_H
#define SIFT_TBL_F2_BLOB_H

#include "postgres.h"

#include "col/f2/store/store.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "core/page.h"

/* STOR[STOR-TBL-10]: wide blob spill threshold 16KiB + batched gen alloc (avoids 8KiB/row frag + 40B ref) */
#define SIFT_TBL_WIDE_THRESHOLD		SIFT_F2_WIDE_THRESHOLD
#define SIFT_TBL_BLOB_MAGIC			0x424C4F42U		/* "BLOB" */
#define SIFT_TBL_BLOB_MAX			((Size) MaxAllocSize)

#ifndef SIFT_COLTAG_WIDE_BLOB
#define SIFT_COLTAG_WIDE_BLOB		0xFE
#endif
#define SIFT_COLTAG_WIDE_RAW		SIFT_F2_TAG_WIDE_RAW

#define SIFT_BLOB_FLAG_LZ4			(UINT16_C(1) << 0)
#define SIFT_BLOB_FLAG_INLINE		(UINT16_C(1) << 1)

typedef struct SiftTblBlobRef
{
	uint32		magic;
	uint32		raw_len;
	uint32		stored_len;
	uint32		crc32c;			/* of the stored bytes */
	uint16		flags;
	uint16		nextents;
	uint32		pad;
	uint64		gen;			/* the generation of the pages: a page of an extent that was reused tells itself */
	SiftExtent	ext[FLEXIBLE_ARRAY_MEMBER];
} SiftTblBlobRef;

StaticAssertDecl(offsetof(SiftTblBlobRef, ext) == 32, "SiftTblBlobRef header is 32 bytes");

#define SIFT_TBL_BLOB_REF_SIZE(n)	(offsetof(SiftTblBlobRef, ext) + (n) * sizeof(SiftExtent))

struct SiftExtVec;
struct SiftF2Store;
struct SiftGranuleView;

/* Writes the wide values the rows hold raw into extents and turns their cells into references; called when a stage is written, before its parts */
extern void sift_tbl_blob_spill_rows(Relation rel, SiftRowBuf *rb);

/* The raw value of a stored reference as a datum of the type, in dst_cx */
extern Datum sift_tbl_blob_read(Relation rel, const char *ref_bytes, uint32 ref_len, Oid typid, int16 typlen, MemoryContext dst_cx);

/* The extents of one stored reference appended to release */
extern void sift_tbl_blob_release_ref(Relation rel, const char *ref_bytes, uint32 ref_len, struct SiftExtVec *release);

/* The extents of the references of the rows dropped (a bit of dropped_mask each) appended to release */
extern void sift_tbl_blob_reclaim(Relation rel, const struct SiftF2Store *store, const struct SiftGranuleView *v, const uint64 *dropped_mask, struct SiftExtVec *release);


#endif							/* SIFT_TBL_F2_BLOB_H */
