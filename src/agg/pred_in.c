/*
 * agg/pred_in.c - the IN lists and lookup tables of the predicates: the hash set of a list, the table by dictionary code or value (agg/pred.c)
 */
#include "postgres.h"

#include "col/kern.h"
#include "agg/pred_in_int.h"

static void
lut_put(uint64 *lut, uint32 code, bool v)
{
	lut[code >> 6] |= (uint64) v << (code & 63);
}

static uint64 *
dict_lut(AggExec *ex, DictSlot *s, const SiftSrcDict *d, SiftAggPred *p)
{
	uint64	   *lut = s->lut[p->id];
	PredState  *st = &ex->ps[p->id];
	MemoryContext old;

	if (lut)
		return lut;
	lut = MemoryContextAllocZero(ex->cxt, ((d->n + 64) / 64) * sizeof(uint64));
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	for (uint32 c = 0; c < d->n; c++)
	{
		Datum		v = text_datum(st->param->paramtype, st->param->paramtypmod, d->bytes + d->off[c],
								   d->off[c + 1] - d->off[c]);

		lut_put(lut, c, eval_bool(ex, st->expr, v, false));
	}
	lut_put(lut, d->n, eval_bool(ex, st->expr, (Datum) 0, true));
	MemoryContextSwitchTo(old);
	s->lut[p->id] = lut;
	return lut;
}

static inline uint64
in_hash64(uint64 x)
{
	x *= UINT64CONST(0x9E3779B97F4A7C15);
	return x ^ (x >> 32);
}

/* the values of a 64-bit IN list in an open-addressing table of at most half full slots */
static void
in_hash_build(PredState *st, const int64 *vals, int nv)
{
	uint64		cap = 16;

	while (cap < (uint64) nv * 2)
		cap <<= 1;
	st->in_hmask = cap - 1;
	st->in_htab = palloc(cap * sizeof(int64));
	st->in_hused = palloc0(cap);
	for (int i = 0; i < nv; i++)
	{
		uint64		h = in_hash64((uint64) vals[i]) & st->in_hmask;

		while (st->in_hused[h] && st->in_htab[h] != vals[i])
			h = (h + 1) & st->in_hmask;
		st->in_hused[h] = 1;
		st->in_htab[h] = vals[i];
	}
}

static bool
in_hash_has(const PredState *st, int64 x)
{
	uint64		h = in_hash64((uint64) x) & st->in_hmask;

	for (; st->in_hused[h]; h = (h + 1) & st->in_hmask)
		if (st->in_htab[h] == x)
			return true;
	return false;
}

/* the test of an IN list of nv values within [lo, hi]: a byte table over the range, else a hash table of the values */
void
in_build(PredState *st, const int64 *vals, int nv, int64 lo, int64 hi)
{
	uint64		span = (uint64) hi - (uint64) lo + 1;	/* 0: all the 2^64 values */

	st->in_lut = palloc0(SIFT_AGG_PAD);
	st->in_nv = nv;
	if (nv == 0)
		return;
	st->in_lo = lo;
	st->in_hi = hi;
	if (span == 0 || span > MAX_RANGE)
	{
		in_hash_build(st, vals, nv);
		return;
	}
	st->in_nlut = (uint32) span;
	st->in_lut = palloc0(span + SIFT_AGG_PAD);
	for (int i = 0; i < nv; i++)
		st->in_lut[vals[i] - lo] = 1;
	st->in_dense = memchr(st->in_lut, 0, span) == NULL;
}

void
eval_in(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);

	if (st->cnull || (p->op && st->in_hasnull))
	{
		clear_words(out, n);	/* x IN NULL, x NOT IN (.., NULL): never true */
		return;
	}
	if (p->op && st->in_empty)
	{
		fill_ones(out, n);
		return;
	}
	if (ex->lay->leaf[p->leaf].vtype == SCV_I64)
	{
		const int64 *x = (const int64 *) v->data + start;

		clear_words(out, n);
		if (st->in_htab)
			for (int i = 0; i < n; i++)
				out[i >> 6] |= (uint64) in_hash_has(st, x[i]) << (i & 63);
		else
			for (int i = 0; i < n; i++)
			{
				uint64		u = (uint64) x[i] - (uint64) st->in_lo;

				out[i >> 6] |= (uint64) (u < st->in_nlut && st->in_lut[u]) << (i & 63);
			}
	}
	else if (st->in_htab)
	{
		const int32 *x = (const int32 *) v->data + start;

		clear_words(out, n);
		for (int i = 0; i < n; i++)
			out[i >> 6] |= (uint64) in_hash_has(st, x[i]) << (i & 63);
	}
	else
		sift_aggk->lut_i32(out, (const int32 *) v->data + start, n, (int32) st->in_lo, st->in_nlut, st->in_lut);
	if (p->op)
		negate(ex, out, n);
	clear_nulls(ex, v, start, n, out);
}

typedef struct LutFill
{
	AggExec    *ex;
	SiftAggPred *p;
} LutFill;

static void
pred_lut_fill(void *arg, int32 v, void *entry)
{
	LutFill    *f = arg;
	PredState  *st = &f->ex->ps[f->p->id];

	*(uint8 *) entry = eval_bool(f->ex, st->expr, leaf_datum(st->param->paramtype, v), false);
}

/* The rows of tmpm, each tested on its own: the values are too far apart for a table (what the statistics did not show) */
static void
lut_rows(AggExec *ex, PredState *st, const int32 *vals, int n, uint64 *out)
{
	Oid			typ = st->param->paramtype;

	clear_words(out, n);
	for (int i = 0; i < n; i++)
		if ((ex->tmpm[i >> 6] >> (i & 63)) & 1 && eval_bool(ex, st->expr, leaf_datum(typ, vals[i]), false))
			out[i >> 6] |= UINT64CONST(1) << (i & 63);
}

void
eval_lut(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];

	if (lf->vtype == SCV_CODE)
	{
		DictSlot   *s = dict_slot(ex, v->dict);
		uint64	   *lut = dict_lut(ex, s, v->dict, p);
		const uint32 *codes = (const uint32 *) v->data + start;

		if (v->isnull)
		{
			for (int i = 0; i < n; i++)
				ex->tmpu32[i] = v->isnull[start + i] ? v->dict->n : codes[i];
			codes = ex->tmpu32;
		}
		sift_colk->lut[SIFT_COLW_32](codes, n, lut, out);
	}
	else
	{
		const int32 *vals = (const int32 *) v->data + start;
		int32		mn,
					mx;
		LutFill		f = {ex, p};
		int			nw = (n + 63) / 64;

		fill_ones(ex->tmpm, n);
		if (v->isnull)
		{
			sift_colk_nonzero(v->isnull + start, n, ex->tmpm2);
			sift_colk->bits_andnot(ex->tmpm, ex->tmpm2, nw);
		}
		if (sift_aggk->minmax_i32(&mn, &mx, vals, ex->tmpm, n))
		{
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

			if (rt_span(&st->rt, mn, mx) > MAX_RANGE)
				lut_rows(ex, st, vals, n, out);
			else
			{
				rt_cover(&st->rt, mn, mx, pred_lut_fill, &f);
				/* a null row reads whatever its slot holds: the result is replaced below */
				sift_aggk->lut_i32(out, vals, n, st->rt.lo, (uint32) ((int64) st->rt.hi - st->rt.lo + 1), (uint8 *) st->rt.tab);
			}
			MemoryContextSwitchTo(old);
		}
		else
			clear_words(out, n);
		if (v->isnull)
		{
			sift_colk_nonzero(v->isnull + start, n, ex->tmpm2);
			sift_colk->bits_andnot(out, ex->tmpm2, nw);
			if (st->null_result)
				sift_colk->bits_or(out, ex->tmpm2, nw);
		}
	}
}
