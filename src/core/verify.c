/*
 * core/verify.c - reference maps of the blocks a directory holds, orphans and the structural check of the store.
 */
#include "postgres.h"

#include "access/transam.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/lmgr.h"

#include "core/store/store.h"
#include "core/store/int.h"
#include "core/u64set.h"
#include "core/visibility/visibility.h"

typedef struct BlockMap
{
	Relation	index;
	BlockNumber hwm;
	Bitmapset  *set;
	SiftExtentVisit visit;		/* sift_store_extent_map: told of every run marked */
	void	   *visit_arg;
	bool		pending;		/* the run in hand is a free run the horizon still holds */
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
	CHECK_FOR_INTERRUPTS();
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
	if (m->visit)
		m->visit(m->visit_arg, what, gen, start, n, m->pending);
}

static bool
mark_freelist_page(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	BlockMap   *m = arg;

	mark(m, blk, 1, "free list page", 0);
	for (uint32 i = 0; i < fl->n; i++)
	{
		m->pending = FullTransactionIdIsValid(fl->ext[i].safexid) && !sift_store_removable(m->index, fl->ext[i].safexid);
		mark(m, fl->ext[i].start, fl->ext[i].nblocks, "free extent", 0);
	}
	m->pending = false;
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

static Bitmapset *
referenced_blocks(Relation index, const SiftDirSnapshot *snap, SiftExtentVisit visit, void *arg)
{
	BlockMap	m = {index, high_water(index), NULL, visit, arg, false};
	Buffer		metabuf;

	for (int i = 0; i < snap->nsegs; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			mark_immutable(&m, &snap->segs[i]);
		else
			mark_wbuf(&m, &snap->segs[i]);
	}
	/* Appenders take pages off the free list while a write buffer chain grows, so it is read last */
	metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	m.hwm = META(BufferGetPage(metabuf))->high_water;
	mark(&m, SIFT_METAPAGE_BLKNO, 2, "metapage and directory", 0);
	for (int i = 0; i < snap->nchain; i++)
		mark(&m, snap->chain[i], 1, "directory chain page", 0);
	sift_freelist_walk(index, metabuf, mark_freelist_page, &m);
	UnlockReleaseBuffer(metabuf);
	return m.set;
}

Bitmapset *
sift_store_referenced_blocks(Relation index, const SiftDirSnapshot *snap)
{
	return referenced_blocks(index, snap, NULL, NULL);
}

/*
 * The directory and the free list are read at two moments. A promotion or merge between them frees blocks that the directory still lists, and they would be
 * counted twice, so a map of the blocks reads both in one hold of the maintenance lock.
 */
static SiftDirSnapshot *
read_view(Relation index)
{
	SIFT_TEST_MUTANT(4, return sift_dir_read(index));
	LockPage(index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	return sift_dir_read(index);
}

static void
release_view(Relation index, SiftDirSnapshot *snap)
{
	pfree(snap);
	SIFT_TEST_MUTANT(4, return);
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
}

void
sift_store_extent_map(Relation index, SiftExtentVisit visit, void *arg)
{
	SiftDirSnapshot *snap = read_view(index);

	bms_free(referenced_blocks(index, snap, visit, arg));
	release_view(index, snap);
}

/* The generations of the segments that are not immutable: a page of one is a buffer's, linked or not yet */
static void
buffer_gens(const SiftDirSnapshot *snap, SiftU64Set *set)
{
	sift_u64set_init(set, snap->nsegs);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			(void) sift_u64set_add(set, snap->segs[i].gen);
}

static bool
in_flight(Page page, const SiftDirSnapshot *snap, const SiftU64Set *buffers)
{
	SiftPageOpaque op;

	if (!sift_page_layout_ok(page))
		return false;
	op = SiftPageGetOpaque(page);
	/* a chain page written by an update that has not flipped yet carries the version it will have */
	if (op->type == SIFT_PT_DIRROOT || op->type == SIFT_PT_DIRLEAF)
		return op->gen > snap->version;
	if (op->type < SIFT_PT_SEGHEADER || op->type > SIFT_PT_WBUFDATA)
		return false;
	if (op->gen >= snap->meta.next_gen)
		return true;
	/*
	 * A page of a write buffer that an appender allocated and has not linked yet carries the generation of its buffer. A page of an immutable segment that
	 * its header does not list is the copy of a move that was killed before it was published (the shrink keeps the generation): an orphan.
	 */
	return sift_u64set_has(buffers, op->gen);
}

Bitmapset *
sift_store_segment_blocks_since(Relation index, const SiftDirSnapshot *snap, const SiftDirSnapshot *before)
{
	BlockMap	m = {index, high_water(index), NULL, NULL, NULL, false};
	SiftU64Set	listed;

	sift_u64set_init(&listed, before->nsegs);
	for (int i = 0; i < before->nsegs; i++)
		(void) sift_u64set_add(&listed, before->segs[i].gen);
	for (int i = 0; i < snap->nsegs; i++)
		if (!sift_u64set_has(&listed, snap->segs[i].gen))
		{
			if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
				mark_immutable(&m, &snap->segs[i]);
			else
				mark_wbuf(&m, &snap->segs[i]);
		}
	sift_u64set_free(&listed);
	return m.set;
}

Bitmapset *
sift_store_orphans(Relation index, SiftDirSnapshot **seen)
{
	SiftDirSnapshot *snap;
	Bitmapset  *ref;
	Bitmapset  *orphans = NULL;
	SiftU64Set	buffers;

	snap = sift_dir_read(index);
	ref = sift_store_referenced_blocks(index, snap);
	buffer_gens(snap, &buffers);
	for (BlockNumber b = FIRST_DATA_BLKNO; b < snap->meta.high_water; b++)
	{
		Buffer		buf;

		if (bms_is_member(b, ref))
			continue;
		buf = read_locked(index, b, BUFFER_LOCK_SHARE);
		if (!in_flight(BufferGetPage(buf), snap, &buffers))
			orphans = bms_add_member(orphans, b);
		UnlockReleaseBuffer(buf);
	}
	bms_free(ref);
	sift_u64set_free(&buffers);
	if (seen != NULL)
		*seen = orphans != NULL ? snap : NULL;
	if (seen == NULL || orphans == NULL)
		pfree(snap);
	return orphans;
}

/* The runs of the blocks of the set, one extent each: the orphans of a killed write are whole extents, so the array follows the runs and not the blocks */
SiftExtent *
sift_store_block_extents(const Bitmapset *set, int *n)
{
	SiftExtent *ext;
	int			x = -1,
				prev = -2,
				runs = 0;

	while ((x = bms_next_member(set, x)) >= 0)
	{
		runs += x != prev + 1;
		prev = x;
	}
	ext = palloc((runs + 1) * sizeof(SiftExtent));
	x = -1;
	*n = 0;
	while ((x = bms_next_member(set, x)) >= 0)
		sift_extent_push(ext, n, x, 1);
	return ext;
}

void
sift_store_verify(Relation index)
{
	SiftDirSnapshot *snap = read_view(index);
	BlockNumber nblocks;
	SiftU64Set	seen;

	SIFT_TEST_GATE(sift_test_check_gate, 19);
	nblocks = RelationGetNumberOfBlocks(index);
	if (nblocks < snap->meta.high_water)
		corrupt(index, "Relation has %u blocks, below the high-water mark %u.", nblocks, snap->meta.high_water);
	sift_u64set_init(&seen, snap->nsegs);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *s = &snap->segs[i];

		if (s->kind < SIFT_SEG_IMMUTABLE || s->kind > SIFT_SEG_VALRUN || s->gen == 0 ||
			s->gen >= snap->meta.next_gen)
			corrupt(index, "Directory entry %d has kind %u generation %llu (next %llu).",
					i, s->kind, ULL(s->gen), ULL(snap->meta.next_gen));
		if (!sift_u64set_add(&seen, s->gen))
			corrupt(index, "Generation %llu appears twice in the directory.", ULL(s->gen));
	}
	sift_u64set_free(&seen);
	bms_free(sift_store_referenced_blocks(index, snap));
	release_view(index, snap);
}
