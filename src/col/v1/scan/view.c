/*
 * col/v1/scan/view.c - what a source read, for the other participants of a parallel scan
 * The items of a parallel scan are ranges of superblocks (the units of a key-cut segment are numbered by key range) and mean what the copy of the
 * directory says up to the end the leader counted, so the participants open their sources on that copy and that end, not on the table as it is.
 */
#include "postgres.h"
#include "col/v1/scan_int.h"

typedef struct ColView
{
	uint32		heap_sbs;
	bool		ends;			/* heap_sbs is the end a leader counted (its keyed units numbered); else the table's, as a source opened now counts them */
	char		dir[FLEXIBLE_ARRAY_MEMBER] pg_attribute_aligned(MAXIMUM_ALIGNOF);	/* the directory: sift_dir_bytes of it */
} ColView;

/* The directory of the index and the end of its table's superblocks, now */
static uint32
view_read(Relation heap, Relation index, bool scope, const SiftKeyRange *keys, SiftDirSnapshot **dir)
{
	BlockNumber nblocks;

	/* the rows this transaction holds back (wbuf/pending.c) are in no page yet, and a worker of a parallel scan has none of its own to flush */
	(void) sift_cwbuf_flush(index);
	nblocks = RelationGetNumberOfBlocks(heap);
	*dir = keys == NULL ? sift_col_dir_read(index, scope) : sift_col_dir_read_range(index, scope, keys);
	return nblocks == 0 ? 0 : sift_block_sb(nblocks - 1) + 1;
}

/* The directory and the end of the source (its superblocks): those another source exported (view), else the table's now (the partitions that may hold keys only). true: the end is a leader's, counted with its keyed units */
bool
scan_view_load(SiftColSource *src, Relation heap, const void *view, const SiftKeyRange *keys)
{
	const ColView *v = view;

	if (v != NULL)
	{
		src->dir = sift_dir_copy((const SiftDirSnapshot *) v->dir);
		src->heap_sbs = v->heap_sbs;
		return v->ends;
	}
	src->heap_sbs = view_read(heap, src->index, (src->flags & SIFT_COLO_SCOPE) != 0, keys, &src->dir);
	if (keys != NULL)
		src->keys = *keys;
	return false;
}

/* The superblocks of the units of the segments a declared key cut are numbered by key range, past the end of the table when it has more of them */
static uint32
keyed_sbs(Relation index, const SiftDirSnapshot *dir, uint32 heap_sbs)
{
	MemoryContext cx = NULL;

	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE || !sift_cseg_keyed(&dir->segs[i]))
			continue;
		if (cx == NULL)
			cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col keyed ends", ALLOCSET_SMALL_SIZES);
		seg = sift_cseg_open(index, &dir->segs[i], cx);
		heap_sbs = Max(heap_sbs, seg->sbdir[seg->nsb - 1].id + 1);
		sift_cseg_close(seg);
	}
	if (cx != NULL)
		MemoryContextDelete(cx);
	return heap_sbs;
}

void *
sift_col_count_view(Relation heap, Relation index, MemoryContext mcxt, Size *len, uint32 *heap_sbs)
{
	SiftDirSnapshot *dir;
	ColView    *v;

	*heap_sbs = view_read(heap, index, false, NULL, &dir);
	*heap_sbs = keyed_sbs(index, dir, *heap_sbs);
	*len = offsetof(ColView, dir) + sift_dir_bytes(dir);
	v = MemoryContextAlloc(mcxt, *len);
	v->heap_sbs = *heap_sbs;
	v->ends = true;
	memcpy(v->dir, dir, sift_dir_bytes(dir));
	pfree(dir);
	return v;
}

bool
sift_col_view_sb_heap(const void *view)
{
	const SiftDirSnapshot *dir = (const SiftDirSnapshot *) ((const ColView *) view)->dir;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && sift_cseg_keyed(&dir->segs[i]))
			return false;
	return true;
}

/* The view of the source of the scope segments that reads the directory dir (read by sift_scope_read), and the end of the table now: the source then reads what the scope read took its rows from */
void *
sift_col_scope_view(Relation heap, const SiftDirSnapshot *dir)
{
	SiftDirSnapshot *own = sift_dir_copy(dir);
	ColView    *v;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);

	sift_col_dir_family(own, true);
	v = palloc(offsetof(ColView, dir) + sift_dir_bytes(own));
	v->heap_sbs = nblocks == 0 ? 0 : sift_block_sb(nblocks - 1) + 1;
	v->ends = false;
	memcpy(v->dir, own, sift_dir_bytes(own));
	pfree(own);
	return v;
}

Size
sift_col_view_size(const SiftColSource *src)
{
	return offsetof(ColView, dir) + sift_dir_bytes(src->dir);
}

void
sift_col_view_export(const SiftColSource *src, void *buf)
{
	ColView    *v = buf;

	v->heap_sbs = src->heap_sbs;
	v->ends = true;
	memcpy(v->dir, src->dir, sift_dir_bytes(src->dir));
}
