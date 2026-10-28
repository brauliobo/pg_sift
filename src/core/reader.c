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
#include "executor/instrument.h"
#include "portability/instr_time.h"
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
	sr->mcxt = CurrentMemoryContext;
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

/* reads of at least this many microseconds, this many of them, start the read-ahead of a reader */
#define SR_SLOW_US		25
#define SR_SLOW_READS	2

static BlockNumber
page_block(const SiftStreamReader *sr, uint32 lpage, int *hint)
{
	return sift_extents_block(sr->extents, sr->pagestart, sr->nextents, lpage, hint);
}

static BlockNumber
sr_stream_cb(ReadStream *stream, void *arg, void *per_buffer_data)
{
	SiftStreamReader *sr = arg;
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;

	while (sr->pf_page >= sr->pf_end)
	{
		uint64		off,
					len;

		if (sr->ahead == NULL || !sr->ahead(sr->ahead_arg, &off, &len))
			return InvalidBlockNumber;
		if (len == 0 || off >= size)
			continue;
		len = Min(len, size - off);
		/* the pages before pf_page were yielded already (ranges may overlap) */
		sr->pf_end = Max((uint32) ((off + len - 1) / SIFT_PAGE_PAYLOAD + 1), sr->pf_page);
		sr->pf_page = Max((uint32) (off / SIFT_PAGE_PAYLOAD), sr->pf_page);
	}
	*(uint32 *) per_buffer_data = sr->pf_page;
	return page_block(sr, sr->pf_page++, &sr->pf_cur);
}

static void
sr_stream_end(SiftStreamReader *sr)
{
	if (BufferIsValid(sr->pf_buf))
		ReleaseBuffer(sr->pf_buf);
	sr->pf_buf = InvalidBuffer;
	if (sr->stream)
		read_stream_end(sr->stream);
	sr->stream = NULL;
}

/* Starts the stream with the pages [first, end), the callback asks sr->ahead for more: the pages before first are the caller's */
static void
sr_stream_begin(SiftStreamReader *sr, uint32 first, uint32 end, int flags)
{
	MemoryContext old;

	sr_stream_end(sr);
	sr->pf_page = first;
	sr->pf_end = end;
	sr->pf_cur = sr->cur;
	/* a get may run in a context that is reset between units: the stream lives as long as the reader */
	old = MemoryContextSwitchTo(sr->mcxt);
	sr->stream = read_stream_begin_relation(flags, NULL, sr->index, MAIN_FORKNUM, sr_stream_cb, sr, sizeof(uint32));
	MemoryContextSwitchTo(old);
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

/*
 * The stream only reads ahead: its buffers come in the order of its pages and go back at once, and the get reads its page through the buffer
 * manager like any other (a hit once the stream has passed the page). The buffer of the first page after lpage is held until a get reaches it.
 */
static Buffer
sr_fetch(SiftStreamReader *sr, uint32 lpage, BlockNumber blk)
{
	Buffer		buf;
	uint64		reads;
	bool		timed;
	instr_time	t0,
				t1;

	if (sr->stream == NULL && sr->cold && sr->ahead != NULL)
		sr_stream_begin(sr, 0, 0, READ_STREAM_DEFAULT);
	while (sr->stream != NULL && lpage < sr->pf_limit)
	{
		void	   *page;

		if (!BufferIsValid(sr->pf_buf))
		{
			sr->pf_buf = read_stream_next_buffer(sr->stream, &page);
			if (!BufferIsValid(sr->pf_buf))
			{
				sr->ahead = NULL;
				sr_stream_end(sr);
				break;
			}
			sr->pf_buf_page = *(uint32 *) page;
		}
		if (sr->pf_buf_page > lpage)
			break;
		ReleaseBuffer(sr->pf_buf);
		sr->pf_buf = InvalidBuffer;
		if (sr->pf_buf_page == lpage)
			break;
	}
	timed = sr->ahead != NULL && !sr->cold;
	reads = pgBufferUsage.shared_blks_read;
	if (timed)
		INSTR_TIME_SET_CURRENT(t0);
	buf = ReadBufferExtended(sr->index, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	if (timed && pgBufferUsage.shared_blks_read != reads)
	{
		INSTR_TIME_SET_CURRENT(t1);
		INSTR_TIME_SUBTRACT(t1, t0);
		/* a page the kernel had read ahead comes in microseconds: only reads that wait for the device show a scan that gains from reading ahead itself */
		if (INSTR_TIME_GET_MICROSEC(t1) >= SR_SLOW_US && ++sr->slow >= SR_SLOW_READS)
		{
			sr->cold = true;
			if (sr->stream == NULL)
				sr_stream_begin(sr, 0, 0, READ_STREAM_DEFAULT);
		}
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

static bool
sr_no_more(void *arg, uint64 *off, uint64 *len)
{
	return false;
}

#ifdef SIFT_TESTING
bool		sift_test_ahead_cold = false;
#endif

void
sift_sr_ahead(SiftStreamReader *sr, SiftSrAheadFn next, void *arg, uint64 upto)
{
#ifdef SIFT_TESTING
	sr->cold |= sift_test_ahead_cold;
#endif
	/* the same source of ranges goes on: a stream in progress is the right one */
	if (next != NULL && next == sr->ahead && arg == sr->ahead_arg)
		return;
	sr_stream_end(sr);
	sr->ahead = effective_io_concurrency > 0 ? next : NULL;
	sr->ahead_arg = arg;
	sr->pf_limit = Min(upto / SIFT_PAGE_PAYLOAD + 1, sr->npages);
}

void
sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len)
{
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;

	sift_sr_ahead(sr, NULL, NULL, 0);
	if (len == 0 || off >= size)
		return;
	len = Min(len, size - off);
	if ((off + len - 1) / SIFT_PAGE_PAYLOAD == off / SIFT_PAGE_PAYLOAD)
		return;
	sr->ahead = sr_no_more;
	sr->pf_limit = sr->npages;
	sr_stream_begin(sr, off / SIFT_PAGE_PAYLOAD, (off + len - 1) / SIFT_PAGE_PAYLOAD + 1, READ_STREAM_FULL);
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
	sift_sr_ahead(sr, NULL, NULL, 0);
	sr->cold = false;
	sr->slow = 0;
}
