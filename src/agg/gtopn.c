/*
 * agg/gtopn.c - ORDER BY an aggregate LIMIT n over the groups of a scan: the groups that cannot be among the first n are dropped before they
 * become tuples. The first sort key is the aggregate; the groups that tie with the n-th stay (PostgreSQL's sort above ranks them by the other keys
 * and applies the LIMIT and OFFSET), so the rows are those of the normal plan whatever the ties.
 */
#include "postgres.h"

#include <math.h>

#include "agg/agg.h"
#include "agg/spec.h"
#include "agg/agg_int.h"
#include "core/limits.h"

typedef struct GKey
{
	sift_i128	v;
	bool		null;
} GKey;

/* float8 in the order of float8_cmp: -0 = 0, every NaN the largest */
static sift_i128
f8_order(double x)
{
	uint64		b;

	if (isnan(x))
		return (sift_i128) UINT64_MAX;
	x += 0.0;
	memcpy(&b, &x, sizeof(b));
	return (sift_i128) ((b & ((uint64) 1 << 63)) ? ~b : (b | ((uint64) 1 << 63)));
}

bool
sift_aggspec_gtopn_kind(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_COUNT_ROWS:
		case SAA_COUNT_DISTINCT:
		case SAA_SUM_INT:
		case SAA_SUM_NUM:
			return true;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			return a->restype == FLOAT8OID;
		default:
			return false;
	}
}

/* the sort value of group g, as the datum of agg_datum would order it; false: a group the raw value cannot rank (a NaN numeric, a float overflow that the normal plan raises) */
static bool
gkey(AggExec *ex, const SiftAggAgg *a, const AggAcc *acc, uint32 g, GKey *k)
{
	k->null = false;
	switch (a->kind)
	{
		case SAA_SUM_INT:
			k->null = acc->n[g] == 0;
			k->v = acc->i[g];
			return true;
		case SAA_SUM_NUM:
			k->null = acc->n[g] == 0;
			k->v = acc->w[g];
			return !acc->nf[g] && acc->i[g] == 0;
		case SAA_SUM_F64:
			k->null = acc->n[g] == 0;
			k->v = f8_order(acc->d[g]);
			return !(isinf(acc->d[g]) && !acc->nf[g]);
		case SAA_AVG_F64:
			k->null = acc->n[g] == 0;
			k->v = k->null ? 0 : f8_order(acc->d[g] / (double) acc->n[g]);
			return true;
		default:
			k->v = acc->n[g];
			return true;
	}
}

/* a is a worse rank than b: later in the sort order of the aggregate */
static inline bool
worse(const SiftAggSpec *s, const GKey *a, const GKey *b)
{
	if (a->null != b->null)
		return a->null != s->gtopn_nulls_first;
	if (a->null)
		return false;
	return s->gtopn_desc ? a->v < b->v : a->v > b->v;
}

static void
sift_down(const SiftAggSpec *s, GKey *h, int n, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1,
					r = l + 1,
					m = i;

		if (l < n && worse(s, &h[l], &h[m]))
			m = l;
		if (r < n && worse(s, &h[r], &h[m]))
			m = r;
		if (m == i)
			return;
		GKey		t = h[i];

		h[i] = h[m];
		h[m] = t;
		i = m;
	}
}

/* drops the groups of the list that rank after the n-th of the sort key (ties stay) */
void
groups_topn(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	AggExec    *t = ex->gtab;
	const SiftAggAgg *a;
	const AggAcc *acc;
	int			n = t->nglist,
				k = s->gtopn_k;
	GKey	   *heap;
	uint32		w = 0;

	if (k <= 0 || s->gtopn_agg < 0 || n <= 2 * k)
		return;
	a = &s->agg[s->gtopn_agg];
	acc = &t->acc[s->gtopn_agg];
	heap = palloc(k * sizeof(GKey));
	for (int i = 0; i < n; i++)
	{
		GKey		key;

		if (!gkey(t, a, acc, t->glist[i], &key))
		{
			pfree(heap);
			return;
		}
		if (i < k)
		{
			heap[i] = key;
			if (i == k - 1)
				for (int j = k / 2 - 1; j >= 0; j--)
					sift_down(s, heap, k, j);
		}
		else if (worse(s, &heap[0], &key))
		{
			heap[0] = key;
			sift_down(s, heap, k, 0);
		}
	}
	for (int i = 0; i < n; i++)
	{
		GKey		key;

		(void) gkey(t, a, acc, t->glist[i], &key);
		if (!worse(s, &key, &heap[0]))
			t->glist[w++] = t->glist[i];
	}
	ex->n_gpruned = n - w;
	t->nglist = w;
	pfree(heap);
}
