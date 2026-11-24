/*
 * agg/v6/par/unnest.c - the pairs of agg/v6/par/unnest.h.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement and the pairs of one slot, not the rows of the table */
#include "postgres.h"

#include "agg/v6/par/unnest.h"
#include "core/par/pool.h"

struct SiftParUnnest
{
	bool		left;
	int			nthreads;
	struct
	{
		SiftParWords *words;
		SiftUnPairs *pairs;
	}			w[SIFT_PAR_MAX_THREADS];
};

bool
sift_par_unnest_shape(const SiftV6Spec *spec, SiftParWordShape *sh)
{
	*sh = (SiftParWordShape) {0};
	return spec->ext != NULL && spec->ext->unnest && spec->ext->ndims == 0 && sift_par_word_shape(spec, SV6SRC_ELEM, true, sh);
}

void
sift_un_slot_pairs(SiftUnPairs *p, const SiftUnSlot *sl, bool left, const SiftParWords *halt)
{
	const SiftArrayLayout lay = {.n_parents = sl->n,.n_elems = sl->nelems,.block_elem_off = sl->block_off,.lengths = sl->lengths,.offsets = sl->offsets};
	const SiftUnnestIn in = {.lay = &lay,.parent_mask = sl->mask,.doc_valid = sl->doc_valid,.left = left};
	SiftUnnestCursor cur = {0};
	uint64		none[SIFT_VEC_WORDS];

	while (!sift_unnest_done(&in, &cur) && (halt == NULL || !sift_par_words_halted(halt)))
	{
		const uint32 m = sift_unnest_expand(&in, &cur, sift_un_pairs_pidx(p), sift_un_pairs_eidx(p), SIFT_VEC_LEN, none);

		if (m > 0)
			sift_un_pairs_emit(p, sl->col, &sl->elem, m);
	}
}

SiftParUnnest *
sift_par_unnest_new(MemoryContext cx, const SiftParWordShape *sh, const SiftV6Pipe *pipe, bool left, int nthreads)
{
	SiftParUnnest *un = MemoryContextAllocZero(cx, sizeof(SiftParUnnest));

	un->left = left;
	un->nthreads = nthreads;
	for (int t = 0; t < nthreads; t++)
	{
		SiftParWords *words = sift_par_words_new(cx, sh, SIFT_UN_PAIRS_MAX);
		SiftV6Pipe *own = sift_v6_pipe_clone(cx, pipe, sift_par_words_sink, words);

		un->w[t].words = words;
		un->w[t].pairs = sift_un_pairs_new(cx, own, pipe->nfact - 1, pipe->type);
	}
	return un;
}

bool
sift_par_unnest_batch(SiftParUnnest *un, int worker, const SiftUnSlot *sl, SiftErr *err, SiftParGroupBatch *out)
{
	SiftParWords *words = un->w[worker].words;

	sift_par_words_reset(words);
	sift_un_slot_pairs(un->w[worker].pairs, sl, un->left, words);
	if (sift_par_words_refused(words, err))
		return false;
	sift_par_words_out(words, out);
	return true;
}
