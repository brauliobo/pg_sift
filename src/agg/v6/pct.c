/*
 * agg/v6/pct.c - percentile_cont in the groups of SiftAggV6 (agg/v6/pct.h).
 */
/* check-memory: allow-allocs the chunks of a state follow the rows of one group, which the work_mem of the statement bounds as the sort of PostgreSQL's own aggregate does */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"

#include "agg/nth.h"
#include "agg/v6/osa/round.h"
#include "agg/v6/pct.h"

#define PCT_CHUNK_FIRST	64
#define PCT_CHUNK_LAST	8192
#define PCT_SORT_AFTER	16384			/* values a state keeps in the arena before they go to its sort */
#define PCT_SORTED		(-1)			/* v1 of a state whose values are in a tuplesort that v0 points to */
#define PCT_DONE		(-2)			/* v1 of a state that has its answer: v0 holds the double */

typedef struct PctChunk
{
	uint32		cap;
	uint32		n;
	uint64		next;			/* the offset of the next chunk + 1, 0 for the last */
} PctChunk;

bool
sift_v6_pct_cont_of(const Aggref *a, double *frac)
{
	const Const *c;
	const TargetEntry *te;
	const SortGroupClause *sgc;
	const char *name = get_func_name(a->aggfnoid);

	if (a->aggkind != AGGKIND_ORDERED_SET || a->aggfilter != NULL || a->aggdistinct != NIL || a->aggvariadic || list_length(a->aggdirectargs) != 1 || list_length(a->args) != 1 ||
		list_length(a->aggorder) != 1 || name == NULL || strcmp(name, "percentile_cont") != 0 || list_length(a->aggargtypes) != 2 || linitial_oid(a->aggargtypes) != FLOAT8OID ||
		lsecond_oid(a->aggargtypes) != FLOAT8OID)
		return false;
	c = linitial(a->aggdirectargs);
	te = linitial(a->args);
	sgc = linitial(a->aggorder);
	if (!IsA(c, Const) || c->constisnull || c->consttype != FLOAT8OID || exprType((Node *) te->expr) != FLOAT8OID || sgc->sortop != Float8LessOperator || sgc->nulls_first)
		return false;
	*frac = DatumGetFloat8(c->constvalue);
	return *frac >= 0.0 && *frac <= 1.0;
}

static inline PctChunk *
chunk_at(const SiftTextArena *ar, uint64 off)
{
	return (PctChunk *) (ar->buf + off);
}

static inline double *
chunk_vals(PctChunk *c)
{
	return (double *) (c + 1);
}

static uint64
chunk_new(SiftTextArena *ar, uint32 cap)
{
	const uint64 at = (ar->used + 7) & ~UINT64CONST(7);
	const uint64 need = at + sizeof(PctChunk) + (uint64) cap * sizeof(double);
	PctChunk   *c;

	if (need > ar->cap)
	{
		ar->cap = Max(ar->cap * 2, Max(need, 4096));
		ar->buf = ar->buf == NULL ? MemoryContextAlloc(ar->cx, ar->cap) : repalloc(ar->buf, ar->cap);
	}
	ar->used = need;
	c = chunk_at(ar, at);
	c->cap = cap;
	c->n = 0;
	c->next = 0;
	return at;
}

void
sift_v6_pct_add(SiftTextArena *ar, V6Acc *acc, double x)
{
	PctChunk   *c;

	if (acc->n == 0)
		acc->v0 = acc->v1 = (int64) chunk_new(ar, PCT_CHUNK_FIRST) + 1;
	c = chunk_at(ar, (uint64) acc->v1 - 1);
	if (c->n == c->cap)
	{
		const uint64 at = chunk_new(ar, Min(c->cap * 2, PCT_CHUNK_LAST));

		chunk_at(ar, (uint64) acc->v1 - 1)->next = at + 1;
		acc->v1 = (int64) at + 1;
		c = chunk_at(ar, at);
	}
	chunk_vals(c)[c->n++] = x;
	acc->n++;
}

static inline Tuplesortstate *
sort_of(const V6Acc *acc)
{
	return (Tuplesortstate *) (uintptr_t) acc->v0;
}

static void
sort_put_chunks(const SiftTextArena *ar, Tuplesortstate *ts, const V6Acc *acc)
{
	for (uint64 off = acc->n > 0 ? acc->v0 : 0; off != 0; off = chunk_at(ar, off - 1)->next)
	{
		PctChunk   *c = chunk_at(ar, off - 1);

		for (uint32 i = 0; i < c->n; i++)
			tuplesort_putdatum(ts, Float8GetDatum(chunk_vals(c)[i]), false);
	}
}

void
sift_v6_pct_add_sorted(SiftTextArena *ar, V6Acc *acc, double x, int sort_kb)
{
	if (acc->v1 != PCT_SORTED && acc->n >= PCT_SORT_AFTER)
	{
		Tuplesortstate *ts = tuplesort_begin_datum(FLOAT8OID, Float8LessOperator, InvalidOid, false, sort_kb, NULL, TUPLESORT_NONE);

		sort_put_chunks(ar, ts, acc);
		acc->v0 = (uint64) (uintptr_t) ts;
		acc->v1 = PCT_SORTED;
	}
	if (acc->v1 == PCT_SORTED)
	{
		tuplesort_putdatum(sort_of(acc), Float8GetDatum(x), false);
		acc->n++;
	}
	else
		sift_v6_pct_add(ar, acc, x);
}

void
sift_v6_pct_merge(SiftTextArena *ar, V6Acc *d, const V6Acc *s)
{
	if (s->n == 0)
		return;
	if (d->v1 == PCT_SORTED)
	{
		sort_put_chunks(ar, sort_of(d), s);
		d->n += s->n;
		return;
	}
	if (d->n == 0)
	{
		*d = *s;
		return;
	}
	chunk_at(ar, (uint64) d->v1 - 1)->next = s->v0;
	d->v1 = s->v1;
	d->n += s->n;
}

void
sift_v6_pct_put(const SiftTextArena *ar, const V6Acc *acc, StringInfo buf)
{
	for (uint64 off = acc->n > 0 ? acc->v0 : 0; off != 0; off = chunk_at(ar, off - 1)->next)
	{
		const PctChunk *c = chunk_at(ar, off - 1);

		appendBinaryStringInfo(buf, (const char *) (c + 1), (int) (c->n * sizeof(double)));
	}
}

const char *
sift_v6_pct_take(SiftTextArena *ar, V6Acc *dst, const V6Acc *src, const char *tail)
{
	for (int64 i = 0; i < src->n; i++, tail += sizeof(double))
	{
		double		x;

		memcpy(&x, tail, sizeof(x));
		sift_v6_pct_add(ar, dst, x);
	}
	return tail;
}

/* the order of PostgreSQL's float8 sort: every NaN is above +Infinity and equal to the others */
static int
float8_order(const void *a, const void *b, void *arg)
{
	const double x = *(const double *) a,
				y = *(const double *) b;

	(void) arg;
	if (isnan(x))
		return isnan(y) ? 0 : 1;
	if (isnan(y))
		return -1;
	return x < y ? -1 : x > y;
}

static double *
gather(const SiftTextArena *ar, const V6Acc *acc)
{
	double	   *v = palloc((Size) acc->n * sizeof(double));
	int64		at = 0;

	for (uint64 off = acc->v0; off != 0; off = chunk_at(ar, off - 1)->next)
	{
		PctChunk   *c = chunk_at(ar, off - 1);

		memcpy(v + at, chunk_vals(c), (Size) c->n * sizeof(double));
		at += c->n;
	}
	return v;
}

static double
sorted_final(Tuplesortstate *ts, int64 n, double frac)
{
	const OsaRound *rd = osa_round(false);
	const double at = frac * (double) (n - 1);
	const int64 lo = (int64) floor(at),
				hi = (int64) ceil(at);
	Datum		first,
				second;
	bool		isnull;

	tuplesort_performsort(ts);
	tuplesort_skiptuples(ts, lo, true);
	tuplesort_getdatum(ts, true, false, &first, &isnull, NULL);
	if (lo == hi)
		return DatumGetFloat8(first);
	tuplesort_getdatum(ts, true, false, &second, &isnull, NULL);
	return osa_lerp(DatumGetFloat8(first), DatumGetFloat8(second), osa_prop(frac, n, lo, rd->prop), rd->lerp);
}

void
sift_v6_pct_finish(V6Acc *acc, double frac)
{
	double		r;

	if (acc->v1 != PCT_SORTED)
		return;
	r = sorted_final(sort_of(acc), acc->n, frac);
	tuplesort_end(sort_of(acc));
	memcpy(&acc->v0, &r, sizeof(r));
	acc->v1 = PCT_DONE;
}

void
sift_v6_pct_release(V6Acc *acc)
{
	if (acc->v1 == PCT_SORTED)
		tuplesort_end(sort_of(acc));
}

double
sift_v6_pct_final(const SiftTextArena *ar, const V6Acc *acc, double frac)
{
	const int64 n = acc->n;
	const OsaRound *rd = osa_round(false);
	const double at = frac * (double) (n - 1);
	const int64 lo = (int64) floor(at),
				hi = (int64) ceil(at);
	double	   *v;
	double		first,
				second,
				r;

	if (acc->v1 == PCT_DONE)
	{
		memcpy(&r, &acc->v0, sizeof(r));
		return r;
	}
	v = gather(ar, acc);

	sift_nth((char *) v, (size_t) n, sizeof(double), (size_t) lo, float8_order, NULL);
	first = v[lo];
	if (lo == hi)
		r = first;
	else
	{
		second = v[lo + 1];
		for (int64 i = lo + 2; i < n; i++)
			if (float8_order(&v[i], &second, NULL) < 0)
				second = v[i];
		r = osa_lerp(first, second, osa_prop(frac, n, lo, rd->prop), rd->lerp);
	}
	pfree(v);
	return r;
}
