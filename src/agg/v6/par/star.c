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

#define SIGN_FLIP		(UINT64CONST(1) << 63)
#define NULL_WORD		UINT64_MAX
#define INLINE_BYTES	12

typedef struct StarW
{
	const SiftParStarShape *sh;
	int			nk;
	int			nfact;
	SiftV6Pipe *pipe;
	SiftVec	  **fact;
	SiftVec    *vec;
	int64	  **data;
	uint64	   *valid;			/* [nfact * SIFT_VEC_WORDS] */
	SiftBatchSel *bs;
	uint32		n;
	bool		serial;
	bool		full;
	uint64	   *key[SIFT_PAR_GROUP_MAX_KW];
	uint64	   *sum[SIFT_PAR_GROUP_MAX_SUM];
	SiftStrRef *str;
	char	   *inl;			/* the bytes of the strings that sit in their views: the batch they were gathered in is gone when the group table reads them */
} StarW;

struct SiftParStar
{
	int			nthreads;
	StarW	   *w[SIFT_PAR_MAX_THREADS];
};

/* ---- the shape ---- */

static bool
int_class(Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	return cls == SV6C_I32 || cls == SV6C_I64;
}

static bool
plain_or_dim(const SiftV6Col *c)
{
	if (c->src == SV6SRC_DIM)
		return c->derive == 0;
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->scale == 0 && c->attno > 0;
}

bool
sift_par_star_shape(const SiftV6Spec *sp, SiftParStarShape *sh)
{
	*sh = (SiftParStarShape) {0};
	if (sp->ext == NULL || sp->ext->unnest || sp->ext->ndims < 1 || sp->ext->kexpr != NIL || sp->ext->resid != NIL || sp->is_table || sp->nparts != 0 || sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfcols != 0 ||
		sp->nfilters != 0 || sp->lock_k != 0 || sp->affine || sp->jsonqual != NIL || OidIsValid(sp->text_indexoid) || sp->naggs < 1 || sp->topk != 0 || sp->nkeys > SIFT_PAR_GROUP_MAX_KW)
		return false;
	for (int d = 0; d < sp->ext->ndims; d++)
		if (sp->ext->dim[d].mode != SV6J_INNER_EQUI || sp->ext->on[d] != NIL)
			return false;
	sh->nk = sp->nkeys;
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Col *c = &sp->key[i];

		if (!plain_or_dim(c))
			return false;
		if (sift_v6_class(c->typid) == SV6C_TEXT && c->src == SV6SRC_DIM && sp->nkeys == 1)
			sh->gs.str = true;
		else if (!int_class(c->typid))
			return false;
	}
	for (int j = 0; j < sp->naggs; j++)
	{
		const SiftV6Agg *a = &sp->agg[j];

		if (a->fclause != 0 || a->nxcols != 0)
			return false;
		if (a->kind == SV6_COUNT_STAR)
			continue;
		if (!plain_or_dim(&a->arg))
			return false;
		if (a->kind == SV6_COUNT && sh->ncount < SIFT_PAR_GROUP_MAX_SUM)
			sh->count_agg[sh->ncount++] = j;
		else if ((a->kind == SV6_SUM || a->kind == SV6_AVG) && (a->arg.typid == INT2OID || a->arg.typid == INT4OID) && sh->nsum < SIFT_PAR_GROUP_MAX_SUM)
			sh->sum_agg[sh->nsum++] = j;
		else
			return false;
	}
	sh->gs.kw = sh->gs.str ? 0 : Max(sp->nkeys, 1);
	sh->gs.nsum = sh->nsum;
	return true;
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

	w->n = 0;
	w->serial = w->full = false;
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
		if (w->serial)
			return sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, "a NULL argument or the NULL key is the scan's"), false;
		if (w->full)
			return sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, 0, "the join gives more rows than the granule has"), false;
	}
	out->n = w->n;
	for (int j = 0; j < SIFT_PAR_GROUP_MAX_KW; j++)
		out->key[j] = w->key[j];
	for (int m = 0; m < w->sh->nsum; m++)
		out->arg[m] = w->sum[m];
	out->str = w->sh->gs.str ? w->str : NULL;
	return true;
}

/* ---- the leader ---- */

static StarW *
worker_new(MemoryContext cx, const SiftParStarShape *sh, const SiftV6Spec *spec, const SiftV6Pipe *pipe)
{
	StarW	   *w = MemoryContextAllocZero(cx, sizeof(StarW));
	const int	nfact = pipe->nfact;

	w->sh = sh;
	w->nk = spec->nkeys;
	w->nfact = nfact;
	w->pipe = sift_v6_pipe_clone(cx, pipe, star_sink, w);
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
	for (int j = 0; j < Max(sh->gs.kw, 1) && !sh->gs.str; j++)
		w->key[j] = MemoryContextAlloc(cx, SIFT_GRANULE_ROWS * sizeof(uint64));
	for (int m = 0; m < sh->nsum; m++)
		w->sum[m] = MemoryContextAlloc(cx, SIFT_GRANULE_ROWS * sizeof(uint64));
	if (sh->gs.str)
	{
		w->str = MemoryContextAlloc(cx, SIFT_GRANULE_ROWS * sizeof(SiftStrRef));
		w->inl = MemoryContextAlloc(cx, (size_t) SIFT_GRANULE_ROWS * INLINE_BYTES);
	}
	return w;
}

SiftParStar *
sift_par_star_new(MemoryContext cx, const SiftParStarShape *sh, const SiftV6Spec *spec, const SiftV6Pipe *pipe, int nthreads)
{
	SiftParStar *st = MemoryContextAllocZero(cx, sizeof(SiftParStar));

	st->nthreads = nthreads;
	for (int t = 0; t < nthreads; t++)
		st->w[t] = worker_new(cx, sh, spec, pipe);
	return st;
}
