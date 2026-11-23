/*
 * stubs.c - the collaborators of src/bm25/eval.c that the PostgreSQL side provides, as this host sees them: the options of
 * the index, the documents the engine knows are dead (its own visibility, instead of a heap snapshot) and the filter of a
 * list of documents as the TID bitmap the shared filter builder reads. No write buffers and no kept docmap directories:
 * sqlite/host/host_stubs.c.
 */
#include "store.h"

#include "bm25/bitmap.h"
#include "bm25/analyze.h"
#include "bm25/eval.h"

int			sift_max_expansions = 512;

/* ---------- options (core/handler.c): the strings of opts sit right after it, as offsets ---------- */

const SiftOptions *
sift_index_options(Relation index)
{
	return &index->store->opts;
}

void
sift_analyzer_config_from_index(Relation index, SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_from_options(sift_index_options(index), cfg);
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	return opts->stemmer ? (const char *) opts + opts->stemmer : "";
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	return opts->fields ? (const char *) opts + opts->fields : "";
}

/* ---------- visibility (core/visibility.c): the engine's dead documents, in ascending lists a page is looked up in ---------- */

/* Calls fn for each dead docid of [lo, hi): one bisection per list, then the run of them in the range */
static void
each_dead(const SiftRel *sr, int64 lo, int64 hi, void (*fn) (int64 docid, void *arg), void *arg)
{
	for (int i = 0; i < 2; i++)
		for (size_t j = lower_bound(sr->dead[i], sr->ndead[i], lo); j < sr->ndead[i] && sr->dead[i][j] < hi; j++)
			fn(sr->dead[i][j], arg);
}

typedef struct DeadPages
{
	int64		lo;
	uint32		dpp;
	uint64	   *pagemask;
} DeadPages;

static void
clear_page(int64 docid, void *arg)
{
	DeadPages  *dp = arg;
	int			page = (int) ((docid - dp->lo) / dp->dpp);

	dp->pagemask[page / 64] &= ~(UINT64CONST(1) << (page & 63));
}

/* A page is all-visible when none of its documents is dead */
void
sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask)
{
	const SiftRel *sr = v->heap->store;
	int64		lo = (int64) sb * SIFT_SB_PAGES * sr->dpp;
	DeadPages	dp = {lo, sr->dpp, pagemask};

	memset(pagemask, 0xFF, SIFT_PAGEMASK_WORDS * sizeof(uint64));
	each_dead(sr, lo, lo + (int64) SIFT_SB_PAGES * sr->dpp, clear_page, &dp);
}

typedef struct DeadOffsets
{
	int64		lo;
	uint64	   *offmask;
} DeadOffsets;

static void
clear_offset(int64 docid, void *arg)
{
	DeadOffsets *dof = arg;
	int			off = (int) (docid - dof->lo);

	dof->offmask[off / 64] &= ~(UINT64CONST(1) << (off & 63));
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	const SiftRel *sr = v->heap->store;
	int64		lo = (int64) blk * sr->dpp;
	DeadOffsets dof = {lo, offmask};

	each_dead(sr, lo, lo + sr->dpp, clear_offset, &dof);
	return (int) sift_offmask_popcount(offmask, 1);
}

/* ---------- the TID bitmap of a list of documents (src/core/compat.h) ---------- */

struct TIDBitmap
{
	const int64 *docids;		/* ascending */
	size_t		n;
	uint32		dpp;
};

struct SiftTbmIter
{
	const TIDBitmap *tbm;
	size_t		next;
};

SiftTbmIter *
sift_tbm_begin(TIDBitmap *tbm)
{
	SiftTbmIter *it = palloc0(sizeof(SiftTbmIter));

	it->tbm = tbm;
	return it;
}

bool
sift_tbm_next(SiftTbmIter *it, BlockNumber *blockno, bool *lossy, OffsetNumber *offs, int *n)
{
	const TIDBitmap *t = it->tbm;
	int64		block;

	if (it->next == t->n)
		return false;
	block = t->docids[it->next] / t->dpp;
	*n = 0;
	for (; it->next < t->n && t->docids[it->next] / t->dpp == block; it->next++)
		offs[(*n)++] = t->docids[it->next] % t->dpp + 1;
	*blockno = block;
	*lossy = false;
	return true;
}

void
sift_tbm_end(SiftTbmIter *it)
{
	pfree(it);
}

SiftFilter *
sift_filter_from_docids(const SiftRel *sr, const int64 *docids, size_t n, MemoryContext mcxt)
{
	TIDBitmap	tbm = {docids, n, sr->dpp};

	return sift_filter_build(&tbm, SIZE_MAX, mcxt);
}
