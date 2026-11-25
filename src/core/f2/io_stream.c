/*
 * core/io_stream.c - the reads of format major 2 through a PostgreSQL ReadStream (core/io_stream.h).
 */
#include "core/host.h"

#include "storage/bufmgr.h"
#include "storage/read_stream.h"

#include "core/compat.h"
#include "core/f2/f2_pace.h"
#include "core/f2/io_stream.h"

#ifdef SIFT_TESTING
bool		sift_test_io_read_stream = true;
#define IO_STREAM_ON	sift_test_io_read_stream
#else
#define IO_STREAM_ON	true
#endif

#define SIFT_IOS_RING_KB		2048
#define SIFT_IOS_RING_MIN_BLOCKS	32

/* The next block of the stream, from the arrays in memory: false at the end. This is all the stream's callback does */
static bool
next_block(SiftExtentReadStream *ios, SiftIoBufMeta *meta)
{
	if (ios->reqs != NULL)
	{
		if (ios->req_cursor >= ios->nreqs)
			return false;
		meta->req_idx = ios->req_cursor;
		meta->blkno = ios->reqs[ios->req_cursor++].blkno;
		return true;
	}
	if (ios->cur_lpage >= ios->end_lpage)
		return false;
	while (ios->ext_cursor + 1 < ios->nextents && ios->pagestart[ios->ext_cursor + 1] <= ios->cur_lpage)
		ios->ext_cursor++;
	meta->req_idx = (uint32) ios->cur_lpage;
	meta->blkno = ios->extents[ios->ext_cursor].start + (BlockNumber) (ios->cur_lpage - ios->pagestart[ios->ext_cursor]);
	ios->cur_lpage++;
	return true;
}

static BlockNumber
stream_cb(ReadStream *stream, void *callback_private_data, void *per_buffer_data)
{
	SiftIoBufMeta *meta = per_buffer_data;

	return next_block(callback_private_data, meta) ? meta->blkno : InvalidBlockNumber;
}

static void
begin(SiftExtentReadStream *ios, SiftIoMode mode, uint64 nblocks)
{
	int			flags;
	bool		ring = nblocks > SIFT_IOS_RING_MIN_BLOCKS;

	switch (mode)
	{
		case SIFT_IOS_FULL_SEQ:
			flags = READ_STREAM_FULL | READ_STREAM_SEQUENTIAL;
			ios->bas = ring ? GetAccessStrategyWithSize(BAS_BULKREAD, SIFT_IOS_RING_KB) : NULL;
			break;
		case SIFT_IOS_MAINT_SEQ:
			flags = READ_STREAM_FULL | READ_STREAM_MAINTENANCE | READ_STREAM_SEQUENTIAL;
			ios->bas = GetAccessStrategy(BAS_VACUUM);
			ios->paced = true;
			break;
		case SIFT_IOS_SELECTIVE:
			flags = READ_STREAM_FULL;
			ios->bas = ring ? GetAccessStrategyWithSize(BAS_BULKREAD, SIFT_IOS_RING_KB) : NULL;
			break;
		default:
			flags = READ_STREAM_DEFAULT;
			ios->bas = NULL;
			break;
	}
	ios->stream = NULL;
	if (IO_STREAM_ON)
		ios->stream = read_stream_begin_relation(flags | SIFT_READ_STREAM_BATCHING, ios->bas, ios->rel, MAIN_FORKNUM, stream_cb, ios, sizeof(SiftIoBufMeta));
}

void
sift_ios_begin_range(SiftExtentReadStream *ios, Relation rel, const SiftExtent *extents, const uint64 *pagestart, uint32 nextents, uint64 first_lpage, uint64 end_lpage,
					 SiftPageType expect_type, uint64 expect_gen, uint32 expect_part_id, SiftIoMode mode)
{
	*ios = (SiftExtentReadStream) {.rel = rel,.expect_type = expect_type,.expect_gen = expect_gen,.expect_part_id = expect_part_id,.extents = extents,.pagestart = pagestart,
		.nextents = nextents,.cur_lpage = first_lpage,.end_lpage = end_lpage};
	while (ios->ext_cursor + 1 < nextents && pagestart[ios->ext_cursor + 1] <= first_lpage)
		ios->ext_cursor++;
	begin(ios, mode, end_lpage - first_lpage);
}

void
sift_ios_begin_reqs(SiftExtentReadStream *ios, Relation rel, const SiftBlockReq *reqs, uint32 nreqs, SiftPageType expect_type, uint64 expect_gen, uint32 expect_part_id, SiftIoMode mode)
{
	*ios = (SiftExtentReadStream) {.rel = rel,.expect_type = expect_type,.expect_gen = expect_gen,.expect_part_id = expect_part_id,.reqs = reqs,.nreqs = nreqs};
	begin(ios, mode, nreqs);
}

Buffer
sift_ios_next(SiftExtentReadStream *ios, SiftIoBufMeta *meta)
{
	Buffer		buf;
	SiftIoBufMeta m;

	if (ios->paced)
		sift_f2_pace_pages(1);
	if (ios->stream != NULL)
	{
		void	   *per_buffer = NULL;

		buf = read_stream_next_buffer(ios->stream, &per_buffer);
		if (!BufferIsValid(buf))
			return InvalidBuffer;
		m = *(SiftIoBufMeta *) per_buffer;
	}
	else
	{
		if (!next_block(ios, &m))
			return InvalidBuffer;
		buf = ReadBufferExtended(ios->rel, MAIN_FORKNUM, m.blkno, RBM_NORMAL, ios->bas);
	}
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_page_expect(ios->rel, BufferGetPage(buf), m.blkno, ios->expect_type, ios->expect_gen, ios->expect_part_id);
	if (meta != NULL)
		*meta = m;
	return buf;
}

void
sift_ios_end(SiftExtentReadStream *ios)
{
	if (ios->stream != NULL)
		read_stream_end(ios->stream);
	if (ios->bas != NULL)
		FreeAccessStrategy(ios->bas);
}

void
sift_ios_exec_slices(Relation rel, const SiftBlockReq *reqs, uint32 nreqs, const SiftCopySlice *slices, char *dst_base, SiftPageType expect_type, uint64 expect_gen, uint32 expect_part_id,
					 SiftIoMode mode)
{
	SiftExtentReadStream ios;
	SiftIoBufMeta meta;
	Buffer		buf;

	sift_ios_begin_reqs(&ios, rel, reqs, nreqs, expect_type, expect_gen, expect_part_id, mode);
	while (BufferIsValid(buf = sift_ios_next(&ios, &meta)))
	{
		const SiftBlockReq *r = &reqs[meta.req_idx];

		for (uint32 s = r->slice_first; s < r->slice_first + r->slice_count; s++)
			memcpy(dst_base + slices[s].dst_off, SiftPageGetData(BufferGetPage(buf)) + slices[s].page_off, slices[s].len);
		UnlockReleaseBuffer(buf);
	}
	sift_ios_end(&ios);
}
