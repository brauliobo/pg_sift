/*
 * sqstubs.c - the PostgreSQL-only collaborators of src/sift_eval.c, as a single-writer embedded database sees them:
 * no heap (every document is visible, no recheck against a snapshot), no mutable segments (documents are
 * buffered in memory by sqbuild.c and written as an immutable segment before they are searched), no parallel scan,
 * no TID bitmap filter. The index options are those of the virtual table.
 */
#include "sqapi.h"

#include "sift_eval.h"
#include "sift_mutable.h"

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

/* ---------- mutable segments (sift_mutable.c): the directory never lists one ---------- */

#define NO_MUTABLE() elog(ERROR, "sift: this build has no mutable segments")

bool
sift_mut_bloom_has(const uint64 *bloom, uint32 hash)
{
	NO_MUTABLE();
	return false;
}

SiftMutReader *
sift_mut_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftMutPageFilter filter, void *filter_arg)
{
	NO_MUTABLE();
	return NULL;
}

bool
sift_mut_next(SiftMutReader *r, SiftMutDoc *doc)
{
	NO_MUTABLE();
	return false;
}

void
sift_mut_totals(const SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl)
{
	NO_MUTABLE();
}

void
sift_mut_close(SiftMutReader *r)
{
	NO_MUTABLE();
}

/* ---------- TID bitmap filters (src/sift_compat.h): only the PostgreSQL planner builds them ---------- */

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
