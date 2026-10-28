/*
 * agg/pred_in.c - the IN lists and lookup tables of the predicates: the hash set of a list, the table by dictionary code or value (agg/pred.c)
 */
#include "postgres.h"

#include "col/kern.h"
#include "core/cmp.h"
#include "core/i64set.h"
#include "core/limits.h"
#include "agg/pred_in_int.h"

static void
lut_put(uint64 *lut, uint32 code, bool v)
{
	lut[code >> 6] |= (uint64) v << (code & 63);
}

/* the entry of code c (n: the NULL input) of the table of predicate p, tested when first asked for */
static void
lut_entry(AggExec *ex, const SiftSrcDict *d, PredState *st, uint64 *lut, uint64 *known, uint32 c)
{
	Datum		v;

	if (known[c >> 6] >> (c & 63) & 1)
		return;
	known[c >> 6] |= UINT64CONST(1) << (c & 63);
	if (c == d->n)
		lut_put(lut, c, eval_bool(ex, st->expr, (Datum) 0, true));
	else
	{
		v = text_datum(st->param->paramtype, st->param->paramtypmod, d->bytes + d->off[c], d->off[c + 1] - d->off[c]);
		lut_put(lut, c, eval_bool(ex, st->expr, v, false));
	}
}

/*
 * The table of the entries of the dictionary for predicate p, one bit each (then the bits of the entries tested, and a last word, set when all
 * are): the entries are tested for the codes of the rows [start, start + n) that still count when the leaves are read for them only (a segment
 * dictionary of 200,000 urls costs a test each, a few rows of a unit need a few), else for all of them.
 */
static uint64 *
dict_lut(AggExec *ex, DictSlot *s, const SiftSrcDict *d, SiftAggPred *p, const uint32 *codes, int start, int n)
{
	uint64	   *lut = s->lut[p->id];
	PredState  *st = &ex->ps[p->id];
	MemoryContext old;
	uint32		nw;
	uint64	   *known;

	d = sift_srcdict_whole(d);
	nw = (d->n + 64) / 64;
	if (lut == NULL)
		lut = s->lut[p->id] = MemoryContextAllocZero(ex->cxt, (2 * nw + 1) * sizeof(uint64));
	if (lut[2 * nw])
		return lut;
	known = lut + nw;
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	if (ex->rsel)
	{
		for (int w = start >> 6; w << 6 < start + n; w++)
			for (uint64 b = ex->rsel[w]; b; b &= b - 1)
			{
				int			r = (w << 6) + pg_rightmost_one_pos64(b);

				if (r >= start + n)
					break;
				lut_entry(ex, d, st, lut, known, codes[r - start]);
			}
	}
	else
	{
		for (uint32 c = 0; c <= d->n; c++)
			lut_entry(ex, d, st, lut, known, c);
		lut[2 * nw] = 1;
	}
	MemoryContextSwitchTo(old);
	return lut;
}

#define MAX_BITS_RANGE	(1 << 28)	/* values a bit table covers (32 MB) */
#define MAX_CACHED_RANGE	(1 << 16)	/* values a byte table covers within the first cache level */
#define MIN_BITS_VALUES	1024		/* members of an IN list from which a range wider than a byte table is a bit table, not a hash table */

static inline bool
in_bit_has(const PredState *st, int64 x)
{
	uint64		u = (uint64) x - (uint64) st->in_lo;

	return u < st->in_nbits && (st->in_bits[u >> 6] >> (u & 63) & 1);
}

typedef enum { IN_HASH, IN_BITS, IN_BYTES } InKind;

/* the structure of the test of an IN list of nv values within a range of span values (0: all the 2^64) */
static InKind
in_kind(uint64 span, int nv)
{
	if (span != 0 && span > MAX_RANGE && span <= MAX_BITS_RANGE && nv >= sift_limit(bits_set_values, MIN_BITS_VALUES))
		return IN_BITS;
	return span == 0 || span > MAX_RANGE ? IN_HASH : IN_BYTES;
}

/* the bytes the scan allocates and clears when it starts for such a list: what a range table costs it, whatever the rows (0: a hash table of the values) */
double
sift_agg_in_table_bytes(uint64 span, int nv)
{
	switch (in_kind(span, nv))
	{
		case IN_BITS:
			return (double) ((span + 63) / 64 + 1) * sizeof(uint64);
		case IN_BYTES:
			return (double) span;
		default:
			return 0;
	}
}

/*
 * the nanoseconds a row costs the test of such a list (measured with the cache the tables of that kind leave: a byte table in the cache 1, a bit table 2,
 * a byte table wider than the cache 10, a hash table 5 for a short list and up to 60 for a long one)
 */
double
sift_agg_in_test_ns(uint64 span, int nv)
{
	switch (in_kind(span, nv))
	{
		case IN_BITS:
			return 2.0;
		case IN_BYTES:
			return span > MAX_CACHED_RANGE ? 10.0 : 1.0;
		default:
			return Min(5.0 + nv / 200.0, 60.0);
	}
}

/*
 * the test of an IN list of nv values within [lo, hi]: a byte table over the range; a bit table when the range is wider and the list long (a
 * random bit costs a cache line like a random byte, but the table is eight times smaller, which keeps it in the cache: 1.5 to 2.4 ns a row
 * from 16 to 64 million values against 8 to 14 for the byte table and 25 to 60 for the hash table of a long list); else a hash table of the values
 */
void
in_build(PredState *st, const int64 *vals, int nv, int64 lo, int64 hi)
{
	uint64		span = (uint64) hi - (uint64) lo + 1;	/* 0: all the 2^64 values */
	InKind		kind = in_kind(span, nv);

	st->in_lut = palloc0(SIFT_AGG_PAD);
	st->in_nv = nv;
	if (nv == 0)
		return;
	st->in_lo = lo;
	st->in_hi = hi;
	if (kind == IN_BITS)
	{
		st->in_nbits = span;
		st->in_bits = palloc0(((span + 63) / 64 + 1) * sizeof(uint64));
		for (int i = 0; i < nv; i++)
			st->in_bits[((uint64) vals[i] - (uint64) lo) >> 6] |= UINT64CONST(1) << (((uint64) vals[i] - (uint64) lo) & 63);
		return;
	}
	if (kind == IN_HASH)
	{
		sift_i64set_build(&st->in_hset, vals, nv);
		return;
	}
	st->in_nlut = (uint32) span;
	st->in_lut = palloc0(span + SIFT_AGG_PAD);
	for (int i = 0; i < nv; i++)
		st->in_lut[vals[i] - lo] = 1;
	st->in_dense = memchr(st->in_lut, 0, span) == NULL;
	if (nv <= ZONE_SET_CAP)
	{
		st->in_vals = palloc(nv * sizeof(int64));
		for (int i = 0; i < nv; i++)
			st->in_vals[i] = vals[i];
		sift_sort_i64(st->in_vals, nv);
		for (int i = 0; i < nv; i++)
			if (st->in_nvals == 0 || st->in_vals[st->in_nvals - 1] != st->in_vals[i])
				st->in_vals[st->in_nvals++] = st->in_vals[i];
	}
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
		if (st->in_hset.slot)
			for (int i = 0; i < n; i++)
				out[i >> 6] |= (uint64) sift_i64set_has(&st->in_hset, x[i]) << (i & 63);
		else if (st->in_bits)
			for (int i = 0; i < n; i++)
				out[i >> 6] |= (uint64) in_bit_has(st, x[i]) << (i & 63);
		else
			for (int i = 0; i < n; i++)
			{
				uint64		u = (uint64) x[i] - (uint64) st->in_lo;

				out[i >> 6] |= (uint64) (u < st->in_nlut && st->in_lut[u]) << (i & 63);
			}
		/* the NaNs of a numeric leaf are stored as overflow rows: NaN equals NaN, and no number */
		if (v->ovf && ex->lay->leaf[p->leaf].typid == NUMERICOID)
			set_rows(v->ovf + start / 64, n, st->in_hasnan, out);
	}
	else if (st->in_hset.slot || st->in_bits)
	{
		const int32 *x = (const int32 *) v->data + start;

		clear_words(out, n);
		if (st->in_bits)
			for (int i = 0; i < n; i++)
				out[i >> 6] |= (uint64) in_bit_has(st, x[i]) << (i & 63);
		else
			for (int i = 0; i < n; i++)
				out[i >> 6] |= (uint64) sift_i64set_has(&st->in_hset, x[i]) << (i & 63);
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
		const uint32 *codes = (const uint32 *) v->data + start;
		uint64	   *lut;

		if (v->isnull)
		{
			for (int i = 0; i < n; i++)
				ex->tmpu32[i] = v->isnull[start + i] ? v->dict->n : codes[i];
			codes = ex->tmpu32;
		}
		lut = dict_lut(ex, s, v->dict, p, codes, start, n);
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
