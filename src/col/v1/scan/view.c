/*
 * col/v1/scan/view.c - what a source read, for the other participants of a parallel scan
 * The items of a parallel scan are ranges of superblocks (the units of a key-cut segment are numbered by key range) and mean what the copy of the
 * directory says up to the end the leader counted, so the participants open their sources on that copy and that end, not on the table as it is.
 */
#include "postgres.h"
#include "col/v1/scan/int.h"

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
