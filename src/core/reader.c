/*
 * core/reader.c - the stream reader: byte ranges of a segment over its extents, with read-ahead.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/nbtxlog.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "catalog/pg_control.h"
#include "common/int.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/bulk_write.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "col/col.h"
#include "bm25/estimate.h"
#include "core/lane.h"
#include "core/store.h"
#include "core/store_int.h"

void
sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents, int nextents,
			 uint64 gen)
{
	uint64		total;

	memset(sr, 0, sizeof(SiftStreamReader));
	sr->index = index;
	sr->extents = extents;
	sr->nextents = nextents;
	sr->gen = gen;
	sr->buf = InvalidBuffer;
	sr->blkno = InvalidBlockNumber;
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	for (int i = 0; i < nextents; i++)
	{
		if (extents[i].nblocks == 0 || extents[i].start < FIRST_DATA_BLKNO ||
			(uint64) extents[i].start + extents[i].nblocks > MaxBlockNumber)
			corrupt(index, "Segment %llu has an invalid extent %u+%u.",
					ULL(gen), extents[i].start, extents[i].nblocks);
	}
	total = sift_extents_pagestart(extents, nextents, sr->pagestart);
	if (total > PG_UINT32_MAX)
		corrupt(index, "Segment %llu spans %llu pages.", ULL(gen), ULL(total));
	sr->npages = total;
}

static BlockNumber
page_block(const SiftStreamReader *sr, uint32 lpage, int *hint)
{
	return sift_extents_block(sr->extents, sr->pagestart, sr->nextents, lpage, hint);
}

static BlockNumber
sr_stream_cb(ReadStream *stream, void *arg, void *per_buffer_data)
{
	SiftStreamReader *sr = arg;

	return sr->pf_issue < sr->pf_end ? page_block(sr, sr->pf_issue++, &sr->pf_cur) : InvalidBlockNumber;
}

static void
sr_unpin(SiftStreamReader *sr)
{
	if (sr->locked)
		LockBuffer(sr->buf, BUFFER_LOCK_UNLOCK);
	if (BufferIsValid(sr->buf))
		ReleaseBuffer(sr->buf);
	sr->locked = false;
	sr->buf = InvalidBuffer;
}

static Buffer
sr_fetch(SiftStreamReader *sr, uint32 lpage, BlockNumber blk)
{
	Buffer		buf;

	if (sr->stream == NULL || lpage < sr->pf_next || lpage >= sr->pf_end)
		return ReadBufferExtended(sr->index, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	for (; sr->pf_next < lpage; sr->pf_next++)
		ReleaseBuffer(read_stream_next_buffer(sr->stream, NULL));
	buf = read_stream_next_buffer(sr->stream, NULL);
	Assert(BufferGetBlockNumber(buf) == blk);
	if (++sr->pf_next == sr->pf_end)
	{
		read_stream_end(sr->stream);
		sr->stream = NULL;
	}
	return buf;
}

/* Bytes [in, in + len) of logical page lpage; the page stays share-locked */
static const char *
sr_bytes(SiftStreamReader *sr, uint32 lpage, size_t in, size_t len)
{
	BlockNumber blk = page_block(sr, lpage, &sr->cur);
	Page		page;

	if (sr->buf == InvalidBuffer || sr->blkno != blk)
	{
		sr_unpin(sr);
		sr->buf = sr_fetch(sr, lpage, blk);
		sr->blkno = blk;
	}
	if (!sr->locked)
	{
		LockBuffer(sr->buf, BUFFER_LOCK_SHARE);
		sr->locked = true;
		sift_store_check_page(sr->index, sr->buf, SIFT_PT_DATA, sr->gen);
	}
	page = BufferGetPage(sr->buf);
	if (in + len > SiftPageGetUsed(page))
		corrupt(sr->index, "Read past the %u written bytes of block %u (segment %llu).",
				SiftPageGetUsed(page), blk, ULL(sr->gen));
	return SiftPageGetData(page) + in;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;
	uint32		lpage = off / SIFT_PAGE_PAYLOAD;
	size_t		in = off % SIFT_PAGE_PAYLOAD;
	char	   *dst = scratch;

	if (len == 0)
		return scratch;
	if (off > size || len > size - off)
		corrupt(sr->index, "Read of %zu bytes at offset %llu is past the end of segment %llu.",
				len, ULL(off), ULL(sr->gen));
	if (in + len <= SIFT_PAGE_PAYLOAD)
		return sr_bytes(sr, lpage, in, len);
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - in);

		memcpy(dst, sr_bytes(sr, lpage++, in, n), n);
		LockBuffer(sr->buf, BUFFER_LOCK_UNLOCK);
		sr->locked = false;
		CHECK_FOR_INTERRUPTS();
		dst += n;
		len -= n;
		in = 0;
	}
	return scratch;
}

void
sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len)
{
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;

	if (sr->stream)
		read_stream_end(sr->stream);
	sr->stream = NULL;
	if (len == 0 || off >= size)
		return;
	len = Min(len, size - off);
	sr->pf_issue = sr->pf_next = off / SIFT_PAGE_PAYLOAD;
	sr->pf_end = (off + len - 1) / SIFT_PAGE_PAYLOAD + 1;
	sr->pf_cur = sr->cur;
	if (sr->pf_end - sr->pf_next > 1)
		sr->stream = read_stream_begin_relation(READ_STREAM_FULL, NULL, sr->index, MAIN_FORKNUM,
												sr_stream_cb, sr, 0);
}

void
sift_sr_unlock(SiftStreamReader *sr)
{
	if (sr->locked)
		LockBuffer(sr->buf, BUFFER_LOCK_UNLOCK);
	sr->locked = false;
}

void
sift_sr_release(SiftStreamReader *sr)
{
	sr_unpin(sr);
	if (sr->stream)
		read_stream_end(sr->stream);
	sr->stream = NULL;
}
