/*
 * stream.c - the stream reader, the write target and the stream writer of core/store.h over the page store of the SQLite table.
 */
#include "store_int.h"

#include <unistd.h>

SQLITE_EXTENSION_INIT3

/* ---------- stream reader ---------- */

void
sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents, int nextents, uint64 gen)
{
	memset(sr, 0, sizeof(*sr));
	sr->index = index;
	sr->extents = extents;
	sr->nextents = nextents;
	sr->gen = gen;
	sr->buf = InvalidBuffer;
	sr->blkno = InvalidBlockNumber;
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	sr->npages = sift_extents_pagestart(extents, nextents, sr->pagestart);
}

/* The reader keeps its current page pinned until it moves to another one or is released */
static Buffer
sr_page(SiftStreamReader *sr, uint32 p)
{
	BlockNumber blk = sift_extents_block(sr->extents, sr->pagestart, sr->nextents, p, &sr->cur);
	Buffer		nb;

	if (sr->buf != InvalidBuffer && sr->blkno == blk)
		return sr->buf;
	nb = ReadBuffer(sr->index, blk);
	sq_unpin(sr->buf);
	sr->buf = nb;
	sr->blkno = blk;
	return nb;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	uint32		p = off / SIFT_PAGE_PAYLOAD;
	size_t		o = off % SIFT_PAGE_PAYLOAD;
	char	   *dst = scratch;

	if (off + len > (uint64) sr->npages * SIFT_PAGE_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("sift table \"%s\": read of %zu bytes at %llu is past the segment", RelationGetRelationName(sr->index),
							   len, (unsigned long long) off)));
	if (o + len <= SIFT_PAGE_PAYLOAD)
		return SiftPageGetData(BufferGetPage(sr_page(sr, p))) + o;
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - o);

		memcpy(dst, SiftPageGetData(BufferGetPage(sr_page(sr, p++))) + o, n);
		dst += n;
		len -= n;
		o = 0;
	}
	return scratch;
}

void
sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len)
{
}

void
sift_sr_unlock(SiftStreamReader *sr)
{
}

void
sift_sr_release(SiftStreamReader *sr)
{
	sq_unpin(sr->buf);
	sr->buf = InvalidBuffer;
	sr->blkno = InvalidBlockNumber;
}

/* ---------- write target and stream writer ---------- */

/* Pages arrive in ascending block order; a unit is inserted as a row (or rows) once its last page is behind us */
struct SiftWriteTarget
{
	SqStore    *st;
	BlockNumber first;
	BlockNumber next;
	int64_t		cur;					/* unit being filled, -1 none */
	uint32		filled;					/* blocks of it written */
	char	   *buf;
};

SiftWriteTarget *
sift_wt_begin(Relation index, SiftWriteMode mode)
{
	SiftWriteTarget *wt = palloc0(sizeof(SiftWriteTarget));
	SqStore    *st = index->store;

	wt->st = st;
	wt->first = wt->next = (st->hw + st->P - 1) / st->P * st->P;
	wt->cur = -1;
	wt->buf = MemoryContextAllocAligned(CurrentMemoryContext, st->unit, 64, 0);
	return wt;
}

static void
fault_check(SqStore *st)
{
	if (st->fault_mode && st->fault_after-- == 0)
	{
		if (st->fault_mode == 2)
			_exit(86);
		st->fault_mode = 0;
		elog(ERROR, "sift: injected fault while writing chunks");
	}
}

static void
unit_flush(SiftWriteTarget *wt)
{
	SqStore    *st = wt->st;
	sqlite3_stmt *q = prep(st, &st->put, "INSERT INTO %s VALUES(?1, ?2)", "chunks");
	uint32		total = wt->filled * SQ_PAGE;

	if (wt->cur < 0)
		return;
	for (uint32 j = 0; j * st->chunk_bytes < total; j++)
	{
		int			rc;

		fault_check(st);
		sqlite3_bind_int64(q, 1, wt->cur * st->R + j);
		sqlite3_bind_blob(q, 2, wt->buf + (size_t) j * st->chunk_bytes, Min((uint32) st->chunk_bytes, total - j * st->chunk_bytes),
						  SQLITE_STATIC);
		rc = sqlite3_step(q);
		sqlite3_reset(q);
		sqlite3_clear_bindings(q);
		CHECK(st, rc);
	}
	wt->cur = -1;
}

void
sift_wt_finish(SiftWriteTarget *wt)
{
	unit_flush(wt);
	wt->st->hw = wt->next;
	wt->st->changed = true;
}

BlockNumber
sq_wt_first(const SiftWriteTarget *wt)
{
	return wt->first;
}

BlockNumber
sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks)
{
	BlockNumber first = wt->next;

	wt->next += nblocks;
	return first;
}

void
sift_wt_put_page(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	SqStore    *st = wt->st;
	int64_t		unit = blkno / st->P;

	if (unit != wt->cur)
	{
		if (unit < wt->cur)
			elog(ERROR, "sift: block %u written out of order", blkno);
		unit_flush(wt);
		wt->cur = unit;
		wt->filled = 0;
		memset(wt->buf, 0, st->unit);
	}
	memcpy(wt->buf + (size_t) (blkno % st->P) * SQ_PAGE, page, SQ_PAGE);
	wt->filled = Max(wt->filled, blkno % st->P + 1);
}

BlockNumber
sift_wt_write_page(SiftWriteTarget *wt, Page page)
{
	BlockNumber blk = wt->next++;

	sift_wt_put_page(wt, blk, page);
	return blk;
}

void
sq_store_set_fault(SqStore *st, int mode, int64_t after_chunks)
{
	st->fault_mode = mode;
	st->fault_after = after_chunks;
}

struct SiftStreamWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	uint64		total;
	uint32		used;
	char	   *page;
	SiftExtentList ext;
};

SiftStreamWriter *
sift_sw_begin(SiftWriteTarget *wt, uint64 gen)
{
	SiftStreamWriter *sw = palloc0(sizeof(SiftStreamWriter));

	sw->wt = wt;
	sw->gen = gen;
	sw->page = MemoryContextAllocAligned(CurrentMemoryContext, BLCKSZ, 64, 0);
	sift_page_init(sw->page, SIFT_PT_DATA, gen);
	sift_extlist_init(&sw->ext);
	return sw;
}

static void
sw_flush(SiftStreamWriter *sw)
{
	BlockNumber blk;

	SiftPageSetUsed(sw->page, sw->used);
	blk = sift_wt_write_page(sw->wt, sw->page);
	sift_extlist_add(&sw->ext, blk);
	sift_page_init(sw->page, SIFT_PT_DATA, sw->gen);
	sw->used = 0;
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	return sw->total;
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	const char *src = data;

	sw->total += len;
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - sw->used);

		memcpy(SiftPageGetData(sw->page) + sw->used, src, n);
		sw->used += n;
		src += n;
		len -= n;
		if (sw->used == SIFT_PAGE_PAYLOAD)
			sw_flush(sw);
	}
}

SiftExtent *
sift_sw_finish(SiftStreamWriter *sw, int *nextents)
{
	if (sw->used > 0 || sw->ext.n == 0)
		sw_flush(sw);
	*nextents = sw->ext.n;
	return sw->ext.ext;
}
