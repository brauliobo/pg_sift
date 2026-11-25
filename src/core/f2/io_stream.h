/*
 * core/io_stream.h - the asynchronous reads of format major 2: the blocks of a part's byte stream, or a list of blocks a caller resolved, delivered by a PostgreSQL ReadStream
 * (look-ahead, combined reads, io_uring or worker AIO on 18 and later, fadvise and vectored reads on 17), instead of one ReadBuffer after another.
 *
 * The callback that tells the stream which block comes next does arithmetic on arrays that are in memory and nothing else (no buffer, no lock, no allocation), which is what
 * SIFT_READ_STREAM_BATCHING needs. A buffer the stream delivers is share-locked and checked as a page of the part (sift_page_expect); the caller copies what it needs and releases it.
 */
#ifndef SIFT_IO_STREAM_H
#define SIFT_IO_STREAM_H

#include "core/host.h"

#include "core/f2/meta.h"
#include "core/page.h"

typedef enum SiftIoMode
{
	SIFT_IOS_FULL_SEQ,			/* the whole byte range of a part or section: sequential, a ring of its own when it is long */
	SIFT_IOS_MAINT_SEQ,			/* a compaction or a vacuum reads it: the same, maintenance look-ahead and the vacuum ring */
	SIFT_IOS_SELECTIVE,			/* the chunks of some columns: blocks that jump, so the stream prefetches each one */
	SIFT_IOS_POINT_BATCH		/* a few blocks that a lookup needs: no ring, they stay in shared buffers */
} SiftIoMode;

/* One block to read and what is copied out of it: slices [slice_first, slice_first + slice_count) of the caller's table */
typedef struct SiftBlockReq
{
	BlockNumber blkno;
	uint32		logical_page;	/* the page of the stream it is, for the checks of the caller */
	uint32		slice_first;
	uint16		slice_count;
	uint16		tag;			/* the caller's: a granule slot, a term */
} SiftBlockReq;

StaticAssertDecl(sizeof(SiftBlockReq) == 16, "SiftBlockReq must be 16 bytes");

/* A run of bytes of a delivered page and where it goes in the caller's buffer */
typedef struct SiftCopySlice
{
	uint16		page_off;		/* from SiftPageGetData(page) */
	uint16		len;
	uint32		dst_off;
} SiftCopySlice;

StaticAssertDecl(sizeof(SiftCopySlice) == 8, "SiftCopySlice must be 8 bytes");

/* Carried with each buffer by the stream: which request it answers */
typedef struct SiftIoBufMeta
{
	uint32		req_idx;		/* the request, or the logical page of a linear stream */
	BlockNumber blkno;
} SiftIoBufMeta;

typedef struct SiftExtentReadStream
{
	Relation	rel;
	SiftPageType expect_type;
	uint64		expect_gen;
	uint32		expect_part_id;

	BufferAccessStrategy bas;
	bool		paced;			/* a read of the maintenance (SIFT_IOS_MAINT_SEQ): each page is charged to the pacer of a paced worker (core/f2/f2_pace.h) */
	struct ReadStream *stream;	/* NULL: every block is read at once, in the order the callback would give them */

	const SiftExtent *extents;	/* a linear stream: the logical pages [cur_lpage, end_lpage) of extents laid one after the other, pagestart[i] the first page of extent i */
	const uint64 *pagestart;
	uint32		nextents;
	uint32		ext_cursor;
	uint64		cur_lpage;
	uint64		end_lpage;

	const SiftBlockReq *reqs;	/* a block-list stream: the blocks in the order of reqs */
	uint32		nreqs;
	uint32		req_cursor;
} SiftExtentReadStream;

/* A linear stream over the logical pages [first_lpage, end_lpage) of the extents of a part */
extern void sift_ios_begin_range(SiftExtentReadStream *ios, Relation rel, const SiftExtent *extents, const uint64 *pagestart, uint32 nextents, uint64 first_lpage, uint64 end_lpage,
								 SiftPageType expect_type, uint64 expect_gen, uint32 expect_part_id, SiftIoMode mode);

/* A stream over blocks the caller resolved */
extern void sift_ios_begin_reqs(SiftExtentReadStream *ios, Relation rel, const SiftBlockReq *reqs, uint32 nreqs, SiftPageType expect_type, uint64 expect_gen, uint32 expect_part_id,
								SiftIoMode mode);

/* The next block, share-locked and checked; InvalidBuffer at the end. The caller releases it (UnlockReleaseBuffer) before it asks for the next one */
extern Buffer sift_ios_next(SiftExtentReadStream *ios, SiftIoBufMeta *meta);

extern void sift_ios_end(SiftExtentReadStream *ios);

/* Every request of reqs read once and its slices copied to dst_base + dst_off */
extern void sift_ios_exec_slices(Relation rel, const SiftBlockReq *reqs, uint32 nreqs, const SiftCopySlice *slices, char *dst_base, SiftPageType expect_type, uint64 expect_gen,
								 uint32 expect_part_id, SiftIoMode mode);

#ifdef SIFT_TESTING
/* sift.test_io_read_stream: off, every block is read with ReadBuffer in the order a stream would ask for it (the answers are the same) */
extern PGDLLIMPORT bool sift_test_io_read_stream;
#endif

#endif							/* SIFT_IO_STREAM_H */
