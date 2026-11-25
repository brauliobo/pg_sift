/*
 * agg/v6/par/wordsink.c - the sink of agg/v6/par/wordsink.h.
 */
/* check-memory: allow-allocs every array here is the rows of one item of a thread, not the rows of the table */
#include "postgres.h"

#include "agg/v6/par/wordsink.h"
#include "agg/v6/pipe.h"
#include "agg/vexpr/str.h"
#include "core/par/pool.h"

#define SIGN_FLIP		(UINT64CONST(1) << 63)
#define NULL_WORD		UINT64_MAX
#define INLINE_BYTES	12

static bool
int_class(Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	return cls == SV6C_I32 || cls == SV6C_I64;
}

static bool
plain_col(const SiftV6Col *c, SiftV6Src via)
{
	if (c->src == via)
		return c->derive == 0 && c->jleaf == 0;
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->scale == 0 && c->attno > 0;
}

bool
sift_par_word_shape(const SiftV6Spec *sp, SiftV6Src via, bool attr_text, SiftParWordShape *sh)
{
	if (sp->is_table || sp->nparts != 0 || sp->rows || sp->nsets != 0 || sp->nbins != 0 || sp->nderive != 0 || sp->nfilters != 0 || sp->lock_k != 0 || sp->affine || sp->jsonqual != NIL ||
		OidIsValid(sp->text_indexoid) || sp->naggs < 1 || sp->naggs > SIFT_PAR_WORD_AGGS || sp->topk != 0 || sp->nkeys > SIFT_PAR_GROUP_MAX_KEYS)
		return false;
	*sh = (SiftParWordShape) {.nk = sp->nkeys};
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Col *c = &sp->key[i];

		if (!plain_col(c, via))
			return false;
		if (sift_v6_class(c->typid) == SV6C_TEXT && (c->src == via || attr_text))
			sh->gs.strs |= 1 << i;
		else if (!int_class(c->typid))
			return false;
	}
	for (int j = 0; j < sp->naggs; j++)
	{
		const SiftV6Agg *a = &sp->agg[j];
		const bool	sums = a->kind == SV6_SUM || a->kind == SV6_AVG;

		sh->n_slot[j] = sh->v_slot[j] = -1;
		if (a->fclause != 0 || a->nxcols != 0)
			return false;
		if (a->kind == SV6_COUNT_STAR)
			continue;
		if (!plain_col(&a->arg, via) || sh->nsum + sums + 1 > SIFT_PAR_GROUP_MAX_SUM)
			return false;
		if (a->kind != SV6_COUNT && !(sums && (a->arg.typid == INT2OID || a->arg.typid == INT4OID)))
			return false;
		sh->slot[sh->nsum] = (SiftParWordSlot) {.agg = j,.valid = true};
		sh->n_slot[j] = sh->nsum++;
		if (sums)
		{
			sh->slot[sh->nsum] = (SiftParWordSlot) {.agg = j};
			sh->v_slot[j] = sh->nsum++;
		}
	}
	sh->gs.nk = Max(sp->nkeys, 1);
	sh->gs.nsum = sh->nsum;
	return true;
}

struct SiftParWords
{
	const SiftParWordShape *sh;
	uint32		cap;
	uint32		n;
	bool		serial;
	bool		full;
	uint64	   *key[SIFT_PAR_GROUP_MAX_KEYS];
	uint64	   *sum[SIFT_PAR_GROUP_MAX_SUM];
	SiftStrRef *str[SIFT_PAR_GROUP_MAX_KEYS];
	char	   *inl[SIFT_PAR_GROUP_MAX_KEYS];	/* the bytes of the strings that sit in their views: the batch they were gathered in is gone when the group table reads them */
};

SiftParWords *
sift_par_words_new(MemoryContext cx, const SiftParWordShape *sh, uint32 cap)
{
	SiftParWords *w = MemoryContextAllocZero(cx, sizeof(SiftParWords));

	w->sh = sh;
	w->cap = cap;
	for (int j = 0; j < sh->gs.nk; j++)
		if (sift_par_group_is_str(&sh->gs, j))
		{
			w->str[j] = MemoryContextAlloc(cx, (size_t) cap * sizeof(SiftStrRef));
			w->inl[j] = MemoryContextAlloc(cx, (size_t) cap * INLINE_BYTES);
		}
		else
			w->key[j] = MemoryContextAlloc(cx, (size_t) cap * sizeof(uint64));
	for (int m = 0; m < sh->nsum; m++)
		w->sum[m] = MemoryContextAlloc(cx, (size_t) cap * sizeof(uint64));
	return w;
}

void
sift_par_words_reset(SiftParWords *w)
{
	w->n = 0;
	w->serial = w->full = false;
}

static void
text_key(SiftParWords *w, int j, uint32 i, const SiftVec *v, uint32 r)
{
	const SiftStrView *sv;

	if (!sift_vec_valid_at(v, r))
	{
		w->str[j][i] = (SiftStrRef) {NULL, SIFT_STR_NULL};
		return;
	}
	sv = &((const SiftStrView *) v->data)[r];
	if (sv->len <= INLINE_BYTES)
	{
		memcpy(w->inl[j] + (size_t) i * INLINE_BYTES, sift_vstr_bytes(sv), sv->len);
		w->str[j][i] = (SiftStrRef) {(const uint8 *) w->inl[j] + (size_t) i * INLINE_BYTES, sv->len};
	}
	else
		w->str[j][i] = (SiftStrRef) {(const uint8 *) sift_vstr_bytes(sv), sv->len};
}

/* false: a key the words cannot say */
static bool
row_words(SiftParWords *w, uint32 i, uint32 r, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	const SiftParWordShape *sh = w->sh;

	if (sh->nk == 0)
		w->key[0][i] = 0;
	for (int j = 0; j < sh->nk; j++)
	{
		const SiftVec *v = keyvec[j];
		uint64		word;
		bool		valid;

		if (sift_par_group_is_str(&sh->gs, j))
		{
			text_key(w, j, i, v, r);
			continue;
		}
		valid = sift_vec_valid_at(v, r);
		word = valid ? (uint64) ((const int64 *) v->data)[r] ^ SIGN_FLIP : NULL_WORD;
		if (valid && word == NULL_WORD)
			return false;
		w->key[j][i] = word;
	}
	for (int m = 0; m < sh->nsum; m++)
	{
		const SiftVec *v = argvec[sh->slot[m].agg];
		const bool	valid = sift_vec_valid_at(v, r);

		w->sum[m][i] = sh->slot[m].valid ? valid : valid ? (uint64) ((const int64 *) v->data)[r] : 0;
	}
	return true;
}

void
sift_par_words_sink(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	SiftParWords *w = arg;
	SiftBatchIter it;
	uint32		r;

	if (w->serial || w->full)
		return;
	if (w->n + bs->n_active > w->cap)
	{
		w->full = true;
		return;
	}
	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		if (!row_words(w, w->n, r, keyvec, argvec))
		{
			w->serial = true;
			return;
		}
		w->n++;
	}
}

bool
sift_par_words_halted(const SiftParWords *w)
{
	return w->serial || w->full;
}

bool
sift_par_words_refused(const SiftParWords *w, SiftErr *err)
{
	if (w->serial)
		return sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, "the NULL key word is the scan's"), true;
	if (w->full)
		return sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, 0, "the pairs of the item are more than the sink holds"), true;
	return false;
}

void
sift_par_words_out(const SiftParWords *w, SiftParGroupBatch *out)
{
	out->n = w->n;
	for (int j = 0; j < SIFT_PAR_GROUP_MAX_KEYS; j++)
	{
		out->key[j] = w->key[j];
		out->str[j] = w->str[j];
	}
	for (int m = 0; m < w->sh->nsum; m++)
		out->arg[m] = w->sum[m];
}
