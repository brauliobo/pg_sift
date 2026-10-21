/*
 * sqstubs.c - the PostgreSQL-only collaborators of src/bm25/eval.c, as a single-writer embedded database sees them:
 * no heap (every document is visible, no recheck against a snapshot), no TID bitmap filter. The index options are those of
 * the virtual table. No write buffers (documents are buffered in memory by sqbuild.c and written as an immutable segment
 * before they are searched) and no kept docmap directories: host/host_stubs.c.
 */
#include "sqapi.h"

#include "bm25/eval.h"

/* ---------- options (sift_handler.c) ---------- */

const SiftOptions *
sift_index_options(Relation index)
{
	return &sq_store_options(index->store)->base;
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	return ((const SqOptions *) opts)->stemmer;
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	return ((const SqOptions *) opts)->fields;
}

/* ---------- visibility (sift_visibility.c): everything the index holds is visible ---------- */

void
sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask)
{
	memset(pagemask, 0xFF, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	int			n = 0;

	for (int i = 0; i < SIFT_OFFMASK_WORDS; i++)
		n += pg_popcount64(offmask[i]);
	return n;
}

/* ---------- TID bitmap filters (src/core/sift_compat.h): only the PostgreSQL planner builds them ---------- */

SiftTbmIter *
sift_tbm_begin(TIDBitmap *tbm)
{
	elog(ERROR, "sift: no TID bitmap filter");
	return NULL;
}

bool
sift_tbm_next(SiftTbmIter *it, BlockNumber *blockno, bool *lossy, OffsetNumber *offs, int *n)
{
	return false;
}

void
sift_tbm_end(SiftTbmIter *it)
{
}
