/*
 * agg/v6/par/star.c - the batches of agg/v6/par/star.h.
 */
/* check-memory: allow-allocs every array here follows the slots of one statement and the rows of a granule, not the rows of the table */
#include "postgres.h"

#include <stdalign.h>

#include "agg/v6/par/star.h"
#include "agg/v6/v6_batch.h"
#include "agg/vexpr_str.h"
#include "col/granule/granule.h"
#include "core/par/pool.h"
#include "core/popcount.h"

typedef struct StarW
{
	int			nfact;
	SiftV6Pipe *pipe;
	SiftVec	  **fact;
	SiftVec    *vec;
	int64	  **data;
	uint64	   *valid;			/* [nfact * SIFT_VEC_WORDS] */
	SiftBatchSel *bs;
	SiftParWords *words;
} StarW;

struct SiftParStar
{
	int			nthreads;
	StarW	   *w[SIFT_PAR_MAX_THREADS];
};

/* ---- the shape ---- */

bool
sift_par_star_shape(const SiftV6Spec *sp, SiftParWordShape *sh)
{
	*sh = (SiftParWordShape) {0};
	if (sp->ext == NULL || sp->ext->unnest || sp->ext->ndims < 1 || sp->ext->kexpr != NIL || sp->ext->resid != NIL || sp->nfcols != 0)
		return false;
	for (int d = 0; d < sp->ext->ndims; d++)
		if (sp->ext->dim[d].mode != SV6J_INNER_EQUI || sp->ext->on[d] != NIL)
			return false;
	return sift_par_word_shape(sp, SV6SRC_DIM, false, sh);
}

/* ---- a thread ---- */

/* The rows of a batch that leaves the last dimension, as words */
static void
star_sink(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	StarW	   *w = arg;
	const SiftParStarShape *sh = w->sh;
	SiftBatchIter it;
	uint32		r;

	if (w->serial || w->full)
		return;
	if (w->n + bs->n_active > SIFT_GRANULE_ROWS)
	{
		w->full = true;
		return;
	}
	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		const uint32 i = w->n;

		if (sh->gs.str)
		{
			const SiftVec *v = keyvec[0];

			if (!sift_vec_valid_at(v, r))
				w->str[i] = (SiftStrRef) {NULL, SIFT_STR_NULL};
			else
			{
				const SiftStrView *sv = &((const SiftStrView *) v->data)[r];

				if (sv->len <= INLINE_BYTES)
				{
					memcpy(w->inl + (size_t) i * INLINE_BYTES, sift_vstr_bytes(sv), sv->len);
					w->str[i] = (SiftStrRef) {(const uint8 *) w->inl + (size_t) i * INLINE_BYTES, sv->len};
				}
				else
					w->str[i] = (SiftStrRef) {(const uint8 *) sift_vstr_bytes(sv), sv->len};
			}
		}
		else if (w->nk == 0)
			w->key[0][i] = 0;
		for (int j = 0; !sh->gs.str && j < w->nk; j++)
		{
			const SiftVec *v = keyvec[j];
			const uint64 word = sift_vec_valid_at(v, r) ? (uint64) ((const int64 *) v->data)[r] ^ SIGN_FLIP : NULL_WORD;

			if (sift_vec_valid_at(v, r) && word == NULL_WORD)
			{
				w->serial = true;
				return;
			}
			w->key[j][i] = word;
		}
		for (int m = 0; m < sh->nsum; m++)
		{
			const SiftVec *v = argvec[sh->sum_agg[m]];

			if (!sift_vec_valid_at(v, r))
			{
				w->serial = true;
				return;
			}
			w->sum[m][i] = (uint64) ((const int64 *) v->data)[r];
		}
		for (int c = 0; c < sh->ncount; c++)
			if (!sift_vec_valid_at(argvec[sh->count_agg[c]], r))
			{
				w->serial = true;
				return;
			}
		w->n++;
	}
}

/* The columns of the slice [at, at + cnt) of the granule as the vectors of a batch (what the scan does for a column of integers) */
static void
slice_vectors(StarW *w, const SiftParCol *fact, int at, int cnt)
{
	for (int i = 0; i < w->nfact; i++)
	{
		uint64	   *valid = w->valid + (size_t) i * SIFT_VEC_WORDS;
		bool		nulls = false;

		memcpy(w->data[i], fact[i].v + at, cnt * sizeof(int64));
		memset(valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
		for (int k = 0; k < cnt; k++)
		{
			valid[k >> 6] |= (uint64) !fact[i].isnull[at + k] << (k & 63);
			nulls |= fact[i].isnull[at + k];
		}
		w->vec[i] = (SiftVec) {.data = w->data[i],.valid = nulls ? valid : NULL,.len = (uint16) cnt,.all_valid = !nulls,.dense = true,.type = SVEC_I64};
	}
}

bool
sift_par_star_batch(SiftParStar *st, int worker, const SiftParCol *fact, const uint64 *active, int n, SiftErr *err, SiftParGroupBatch *out)
{
	StarW	   *w = st->w[worker];
	SiftBatchSel *bs = w->bs;

	sift_par_words_reset(w->words);
	for (int at = 0; at < n; at += SIFT_VEC_LEN)
	{
		const int	cnt = Min(SIFT_VEC_LEN, n - at),
					words = (cnt + 63) / 64;

		memset(bs->mask, 0, sizeof(bs->mask));
		memcpy(bs->mask, active + at / 64, words * sizeof(uint64));
		if (cnt & 63)
			bs->mask[words - 1] &= (UINT64CONST(1) << (cnt & 63)) - 1;
		bs->n_raw = (uint16) cnt;
		bs->n_active = 0;
		for (int k = 0; k < words; k++)
			bs->n_active += (uint16) sift_pc64(bs->mask[k]);
		bs->all_active = false;
		bs->has_sel = false;
		if (bs->n_active == 0)
			continue;
		slice_vectors(w, fact, at, cnt);
		sift_v6_pipe_run(w->pipe, w->fact, bs);
		if (sift_par_words_refused(w->words, err))
			return false;
	}
	sift_par_words_out(w->words, out);
	return true;
}

/* ---- the leader ---- */

static StarW *
worker_new(MemoryContext cx, const SiftParWordShape *sh, const SiftV6Pipe *pipe)
{
	StarW	   *w = MemoryContextAllocZero(cx, sizeof(StarW));
	const int	nfact = pipe->nfact;

	w->nfact = nfact;
	w->words = sift_par_words_new(cx, sh, SIFT_GRANULE_ROWS);
	w->pipe = sift_v6_pipe_clone(cx, pipe, sift_par_words_sink, w->words);
	w->fact = MemoryContextAlloc(cx, Max(nfact, 1) * sizeof(SiftVec *));
	w->vec = MemoryContextAllocZero(cx, Max(nfact, 1) * sizeof(SiftVec));
	w->data = MemoryContextAlloc(cx, Max(nfact, 1) * sizeof(int64 *));
	w->valid = MemoryContextAllocZero(cx, Max(nfact, 1) * SIFT_VEC_WORDS * sizeof(uint64));
	w->bs = MemoryContextAllocAligned(cx, sizeof(SiftBatchSel), alignof(SiftBatchSel), MCXT_ALLOC_ZERO);
	for (int i = 0; i < nfact; i++)
	{
		w->data[i] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		w->fact[i] = &w->vec[i];
	}
	return w;
}

SiftParStar *
sift_par_star_new(MemoryContext cx, const SiftParWordShape *sh, const SiftV6Pipe *pipe, int nthreads)
{
	SiftParStar *st = MemoryContextAllocZero(cx, sizeof(SiftParStar));

	st->nthreads = nthreads;
	for (int t = 0; t < nthreads; t++)
		st->w[t] = worker_new(cx, sh, pipe);
	return st;
}
