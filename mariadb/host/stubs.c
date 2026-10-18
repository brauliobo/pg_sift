/*
 * stubs.c - the collaborators of src/sift_eval.c that the PostgreSQL side provides, as this host sees them: the options of
 * the index, the documents the engine knows are dead (its own visibility, instead of a heap snapshot), no mutable segments
 * (the live segment of a query is an immutable one built from the queue) and the filter of a list of documents as the
 * TID bitmap the shared filter builder reads.
 */
#include "store.h"

#include "sift_eval.h"
#include "sift_mutable.h"

int			sift_max_expansions = 512;

/* ---------- options (sift_handler.c): the strings of opts sit right after it, as offsets ---------- */

const SiftOptions *
sift_index_options(Relation index)
{
	return &index->store->opts;
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

/* ---------- visibility (sift_visibility.c): the engine's dead documents ---------- */

void
sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask)
{
	memset(pagemask, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	const SiftRel *sr = v->heap->store;
	int			n = 0;

	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
	{
		for (uint64 m = offmask[w]; sr->any_dead && m; m &= m - 1)
		{
			int			off = w * 64 + __builtin_ctzll(m) + 1;

			if (sr->is_dead(sr->dead_ctx, (int64) blk * sr->dpp + off - 1))
				offmask[w] &= ~(m & -m);
		}
		n += pg_popcount64(offmask[w]);
	}
	return n;
}

/* ---------- mutable segments (sift_mutable.c): the directory never lists one ---------- */

#define NO_MUTABLE() elog(ERROR, "sift: this build has no mutable segments")

bool
sift_mut_bloom_has(const uint64 *bloom, uint32 hash)
{
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
	return false;
}

void
sift_mut_totals(const SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl)
{
}

void
sift_mut_close(SiftMutReader *r)
{
}

/* ---------- the TID bitmap of a list of documents (src/sift_compat.h) ---------- */

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
