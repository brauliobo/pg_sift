/*
 * core/verify.c - reference maps of the blocks a directory holds, orphans and the structural check of the store.
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

typedef struct BlockMap
{
	Relation	index;
	BlockNumber hwm;
	Bitmapset  *set;
} BlockMap;

static BlockNumber
high_water(Relation index)
{
	Buffer		metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	BlockNumber hwm = META(BufferGetPage(metabuf))->high_water;

	UnlockReleaseBuffer(metabuf);
	return hwm;
}

static void
mark(BlockMap *m, BlockNumber start, uint64 n, const char *what, uint64 gen)
{
	if (n == 0 || start + n > m->hwm)
		corrupt(m->index, "The %s of generation %llu (blocks %u+%llu) lies past the high-water mark %u.",
				what, ULL(gen), start, ULL(n), m->hwm);
	for (BlockNumber b = start; b < start + n; b++)
		if (bms_is_member(b, m->set))
		{
			if (gen == 0)
				corrupt(m->index, "Block %u is referenced twice (%s).", b, what);
			corrupt(m->index, "Block %u is referenced twice (%s of generation %llu).", b, what, ULL(gen));
		}
	m->set = bms_add_range(m->set, start, start + n - 1);
}

static bool
mark_freelist_page(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	BlockMap   *m = arg;

	mark(m, blk, 1, "free list page", 0);
	for (uint32 i = 0; i < fl->n; i++)
		mark(m, fl->ext[i].start, fl->ext[i].nblocks, "free extent", 0);
	return true;
}

/* The extents and the liveness run of a segment, read from its header page */
static SiftExtent *
read_seg_extents(Relation index, const SiftSegEntry *s, uint32 *n, BlockNumber *live_first,
				 uint32 *live_npages)
{
	Buffer		buf = read_locked(index, s->header_blk, BUFFER_LOCK_SHARE);
	Page		page;
	SiftSegHeaderData *hdr;
	SiftExtent *ext;

	sift_store_check_page(index, buf, SIFT_PT_SEGHEADER, s->gen);
	page = BufferGetPage(buf);
	hdr = (SiftSegHeaderData *) SiftPageGetData(page);
	*n = hdr->nextents;
	if (*n > SIFT_MAX_EXTENTS || SiftPageGetUsed(page) < offsetof(SiftSegHeaderData, extents) + *n * sizeof(SiftExtent))
		corrupt(index, "Segment header block %u lists %u extents in %u bytes.",
				s->header_blk, *n, SiftPageGetUsed(page));
	ext = palloc(Max(*n, 1) * sizeof(SiftExtent));
	memcpy(ext, hdr->extents, *n * sizeof(SiftExtent));
	*live_first = hdr->live_first;
	*live_npages = hdr->live_npages;
	UnlockReleaseBuffer(buf);
	return ext;
}

static void
mark_immutable(BlockMap *m, const SiftSegEntry *s)
{
	SiftExtent *ext;
	uint32		n;
	BlockNumber live_first;
	uint32		live_npages;

	mark(m, s->header_blk, 1, "segment header", s->gen);
	ext = read_seg_extents(m->index, s, &n, &live_first, &live_npages);
	for (uint32 i = 0; i < n; i++)
		mark(m, ext[i].start, ext[i].nblocks, "segment extent", s->gen);
	if (live_npages > 0)
		mark(m, live_first, live_npages, "liveness run", s->gen);
	pfree(ext);
}

SiftExtent *
sift_store_immutable_extents(Relation index, const SiftSegEntry *s, int *nextents)
{
	uint32		n;
	BlockNumber live_first;
	uint32		live_npages;
	SiftExtent *ext = read_seg_extents(index, s, &n, &live_first, &live_npages);

	ext = repalloc(ext, (n + 2) * sizeof(SiftExtent));
	ext[n++] = (SiftExtent) {s->header_blk, 1};
	if (live_npages > 0)
		ext[n++] = (SiftExtent) {live_first, live_npages};
	*nextents = n;
	return ext;
}

static void
mark_wbuf(BlockMap *m, const SiftSegEntry *s)
{
	Buffer		buf;
	SiftWbufHeaderData hdr;

	mark(m, s->header_blk, 1, "write buffer header", s->gen);
	buf = read_locked(m->index, s->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(m->index, buf, SIFT_PT_WBUFHEADER, s->gen);
	if (SiftPageGetUsed(BufferGetPage(buf)) < sizeof(SiftWbufHeaderData))
		corrupt(m->index, "Write buffer header block %u is truncated.", s->header_blk);
	hdr = *(SiftWbufHeaderData *) SiftPageGetData(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = hdr.lanes[l].head;

		while (BlockNumberIsValid(blk))
		{
			/* mark() rejects a revisit, which also ends a cycle */
			if (blk >= m->hwm)
				m->hwm = high_water(m->index);
			mark(m, blk, 1, "write buffer page", s->gen);
			buf = read_locked(m->index, blk, BUFFER_LOCK_SHARE);
			sift_store_check_page(m->index, buf, SIFT_PT_WBUFDATA, s->gen);
			blk = ((SiftWbufPageHeaderData *) SiftPageGetData(BufferGetPage(buf)))->next;
			UnlockReleaseBuffer(buf);
		}
	}
}

Bitmapset *
sift_store_referenced_blocks(Relation index, const SiftDirSnapshot *snap)
{
	BlockMap	m = {index, high_water(index), NULL};
	Buffer		metabuf;

	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			mark_immutable(&m, &snap->segs[i]);
		else
			mark_wbuf(&m, &snap->segs[i]);
	}
	/* Appenders take pages off the free list while a write buffer chain grows, so it is read last */
	metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	m.hwm = META(BufferGetPage(metabuf))->high_water;
	mark(&m, SIFT_METAPAGE_BLKNO, 2, "metapage and directory", 0);
	sift_freelist_walk(index, metabuf, mark_freelist_page, &m);
	UnlockReleaseBuffer(metabuf);
	return m.set;
}

static bool
in_flight(Page page, const SiftDirSnapshot *snap)
{
	SiftPageOpaque op;

	if (!layout_ok(page))
		return false;
	op = SiftPageGetOpaque(page);
	if (op->type < SIFT_PT_SEGHEADER || op->type > SIFT_PT_WBUFDATA)
		return false;
	if (op->gen >= snap->meta.next_gen)
		return true;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == op->gen)
			return true;
	return false;
}

Bitmapset *
sift_store_orphans(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Bitmapset  *ref;
	Bitmapset  *orphans = NULL;

	sift_dir_read(index, snap);
	ref = sift_store_referenced_blocks(index, snap);
	for (BlockNumber b = FIRST_DATA_BLKNO; b < snap->meta.high_water; b++)
	{
		Buffer		buf;

		if (bms_is_member(b, ref))
			continue;
		buf = read_locked(index, b, BUFFER_LOCK_SHARE);
		if (!in_flight(BufferGetPage(buf), snap))
			orphans = bms_add_member(orphans, b);
		UnlockReleaseBuffer(buf);
	}
	bms_free(ref);
	pfree(snap);
	return orphans;
}

void
sift_store_verify(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	BlockNumber nblocks;

	sift_dir_read(index, snap);
	nblocks = RelationGetNumberOfBlocks(index);
	if (nblocks < snap->meta.high_water)
		corrupt(index, "Relation has %u blocks, below the high-water mark %u.", nblocks, snap->meta.high_water);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *s = &snap->segs[i];

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_WBUF_SEALED || s->gen == 0 ||
			s->gen >= snap->meta.next_gen)
			corrupt(index, "Directory entry %d has kind %u generation %llu (next %llu).",
					i, s->kind, ULL(s->gen), ULL(snap->meta.next_gen));
		for (int j = 0; j < i; j++)
			if (snap->segs[j].gen == s->gen)
				corrupt(index, "Generation %llu appears twice in the directory.", ULL(s->gen));
	}
	bms_free(sift_store_referenced_blocks(index, snap));
	pfree(snap);
}
