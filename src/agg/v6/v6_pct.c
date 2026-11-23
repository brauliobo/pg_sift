/*
 * agg/v6/v6_pct.c - percentile_cont in the groups of SiftAggV6 (agg/v6/v6_pct.h).
 */
/* check-memory: allow-allocs the chunks of a state follow the rows of one group, which the work_mem of the statement bounds as the sort of PostgreSQL's own aggregate does */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/nth.h"
#include "agg/osa_int.h"
#include "agg/v6/v6_pct.h"

#define PCT_CHUNK_FIRST	64
#define PCT_CHUNK_LAST	8192

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

void
sift_v6_pct_merge(SiftTextArena *ar, V6Acc *d, const V6Acc *s)
{
	if (s->n == 0)
		return;
	if (d->n == 0)
	{
		*d = *s;
		return;
	}
	chunk_at(ar, (uint64) d->v1 - 1)->next = s->v0;
	d->v1 = s->v1;
	d->n += s->n;
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

double
sift_v6_pct_final(const SiftTextArena *ar, const V6Acc *acc, double frac)
{
	const int64 n = acc->n;
	const OsaRound *rd = osa_round(false);
	const double at = frac * (double) (n - 1);
	const int64 lo = (int64) floor(at),
				hi = (int64) ceil(at);
	double	   *v = gather(ar, acc);
	double		first,
				second,
				r;

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
