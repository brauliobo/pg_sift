/*
 * col/f2/agg/proj/merge.c - the k-way merge of sorted sources and the bounded heap of unsorted ones (col/f2/agg/proj/merge.h).
 */
#include "core/host.h"

#include <math.h>

#include "col/f2/agg/proj/merge.h"

static int
f8_cmp(double a, double b)
{
	if (isnan(a) || isnan(b))
		return isnan(a) == isnan(b) ? 0 : (isnan(a) ? 1 : -1);
	return a < b ? -1 : a > b;
}

static int
value_cmp(SiftCellKind kind, const SiftCell *a, const SiftCell *b)
{
	uint32		n = Min(a->slen, b->slen);
	int			r;

	if (kind == SIFT_CELL_F8)
		return f8_cmp(a->f, b->f);
	if (kind == SIFT_CELL_STR)
	{
		r = n > 0 ? memcmp(a->s, b->s, n) : 0;
		return r != 0 ? r : (a->slen < b->slen ? -1 : a->slen > b->slen);
	}
	return a->i < b->i ? -1 : a->i > b->i;
}

int
sift_proj_order_cmp(const SiftProjSpec *spec, const SiftProjColumns *cols, const SiftCell *a, const SiftCell *b)
{
	for (int k = 0; k < spec->nkeys; k++)
	{
		const SiftProjKey *key = &spec->key[k];
		const SiftCell *x = &a[key->leaf],
				   *y = &b[key->leaf];
		int			r;

		if (x->isnull || y->isnull)
		{
			if (x->isnull == y->isnull)
				continue;
			return x->isnull == key->nulls_first ? -1 : 1;
		}
		r = value_cmp(cols->col[key->leaf].kind, x, y);
		if (r != 0)
			return key->desc ? -r : r;
	}
	return 0;
}

/* ---- the merge ---- */

typedef struct Head
{
	SiftProjStream src;
	bool		has;
	uint64		tid;
	const SiftCell *row;
} Head;

struct SiftProjMerge
{
	const SiftProjSpec *spec;
	const SiftProjColumns *cols;
	int64		limit;
	int64		given;
	Head	   *head;
	int			n;
	int			cap;
	int			last;			/* the source whose row was given last: it moves on at the next call */
};

SiftProjMerge *
sift_proj_merge_new(const SiftProjSpec *spec, const SiftProjColumns *cols, int64 limit)
{
	SiftProjMerge *m = palloc0(sizeof(SiftProjMerge));

	*m = (SiftProjMerge) {.spec = spec,.cols = cols,.limit = limit,.last = -1};
	return m;
}

static void
advance(Head *h)
{
	h->has = h->src.next(h->src.arg, &h->tid, &h->row);
}

void
sift_proj_merge_add(SiftProjMerge *m, SiftProjStream s)
{
	if (m->n == m->cap)
	{
		m->cap = m->cap == 0 ? 8 : m->cap * 2;
		m->head = m->head == NULL ? palloc(m->cap * sizeof(Head)) : repalloc(m->head, m->cap * sizeof(Head));
	}
	m->head[m->n] = (Head) {.src = s};
	advance(&m->head[m->n++]);
}

bool
sift_proj_merge_next(SiftProjMerge *m, uint64 *tid, const SiftCell **row)
{
	int			best = -1;

	if (m->limit >= 0 && m->given >= m->limit)
		return false;
	if (m->last >= 0)
		advance(&m->head[m->last]);
	for (int i = 0; i < m->n; i++)
	{
		int			c;

		if (!m->head[i].has)
			continue;
		if (best < 0)
		{
			best = i;
			continue;
		}
		c = sift_proj_order_cmp(m->spec, m->cols, m->head[i].row, m->head[best].row);
		if (c < 0 || (c == 0 && m->head[i].tid < m->head[best].tid))
			best = i;
	}
	m->last = best;
	if (best < 0)
		return false;
	*tid = m->head[best].tid;
	*row = m->head[best].row;
	m->given++;
	return true;
}

void
sift_proj_merge_free(SiftProjMerge *m)
{
	if (m->head != NULL)
		pfree(m->head);
	pfree(m);
}

/* ---- the bounded heap of unsorted rows ---- */

typedef struct Kept
{
	uint64		tid;
	SiftCell   *cell;
	char	  **str;			/* the copies of the strings the cells point to */
	int			nstr;
} Kept;

struct SiftProjTopK
{
	const SiftProjSpec *spec;
	const SiftProjColumns *cols;
	int			ncols;
	int64		limit;
	Kept	   *kept;			/* a heap with the last row of the order at the top, until it is sorted */
	int			n;
	int			cap;
	bool		sorted;
	int			next;
};

SiftProjTopK *
sift_proj_topk_new(const SiftProjSpec *spec, const SiftProjColumns *cols, int ncols, int64 limit)
{
	SiftProjTopK *t = palloc0(sizeof(SiftProjTopK));

	*t = (SiftProjTopK) {.spec = spec,.cols = cols,.ncols = ncols,.limit = limit};
	return t;
}

static int
kept_cmp(const SiftProjTopK *t, const Kept *a, const Kept *b)
{
	int			c = sift_proj_order_cmp(t->spec, t->cols, a->cell, b->cell);

	return c != 0 ? c : (a->tid < b->tid ? -1 : a->tid > b->tid);
}

static Kept
kept_copy(const SiftProjTopK *t, uint64 tid, const SiftCell *row)
{
	Kept		k = {.tid = tid,.cell = palloc(Max(t->ncols, 1) * sizeof(SiftCell))};

	memcpy(k.cell, row, t->ncols * sizeof(SiftCell));
	for (int c = 0; c < t->ncols; c++)
	{
		k.cell[c].xp = NULL;
		k.cell[c].xplen = 0;
		if (!row[c].isnull && row[c].s != NULL)
			k.nstr++;
	}
	/* check-memory: allow the strings are the constants of one qual */
	k.str = palloc(Max(k.nstr, 1) * sizeof(char *));
	k.nstr = 0;
	for (int c = 0; c < t->ncols; c++)
		if (!row[c].isnull && row[c].s != NULL)
		{
			char	   *p = palloc(Max(row[c].slen, 1));

			memcpy(p, row[c].s, row[c].slen);
			k.cell[c].s = p;
			k.str[k.nstr++] = p;
		}
	return k;
}

static void
kept_free(Kept *k)
{
	for (int i = 0; i < k->nstr; i++)
		pfree(k->str[i]);
	pfree(k->str);
	pfree(k->cell);
}

static void
sift_down(SiftProjTopK *t, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1,
					r = l + 1,
					big = i;
		Kept		tmp;

		if (l < t->n && kept_cmp(t, &t->kept[l], &t->kept[big]) > 0)
			big = l;
		if (r < t->n && kept_cmp(t, &t->kept[r], &t->kept[big]) > 0)
			big = r;
		if (big == i)
			return;
		tmp = t->kept[i];
		t->kept[i] = t->kept[big];
		t->kept[big] = tmp;
		i = big;
	}
}

void
sift_proj_topk_add(SiftProjTopK *t, uint64 tid, const SiftCell *row)
{
	Kept		k;

	Assert(!t->sorted);
	if (t->limit == 0)
		return;
	if (t->limit >= 0 && t->n == t->limit)
	{
		Kept		probe = {.tid = tid,.cell = (SiftCell *) row};

		if (kept_cmp(t, &probe, &t->kept[0]) >= 0)
			return;
		kept_free(&t->kept[0]);
		t->kept[0] = kept_copy(t, tid, row);
		sift_down(t, 0);
		return;
	}
	if (t->n == t->cap)
	{
		t->cap = t->cap == 0 ? 16 : t->cap * 2;
		t->kept = t->kept == NULL ? palloc(t->cap * sizeof(Kept)) : repalloc(t->kept, t->cap * sizeof(Kept));
	}
	k = kept_copy(t, tid, row);
	t->kept[t->n] = k;
	for (int i = t->n++; i > 0 && kept_cmp(t, &t->kept[i], &t->kept[(i - 1) / 2]) > 0; i = (i - 1) / 2)
	{
		Kept		tmp = t->kept[i];

		t->kept[i] = t->kept[(i - 1) / 2];
		t->kept[(i - 1) / 2] = tmp;
	}
}

int
sift_proj_topk_count(const SiftProjTopK *t)
{
	return t->n;
}

static bool
topk_next(void *arg, uint64 *tid, const SiftCell **row)
{
	SiftProjTopK *t = arg;

	if (t->next >= t->n)
		return false;
	*tid = t->kept[t->next].tid;
	*row = t->kept[t->next++].cell;
	return true;
}

static int
sort_cmp(const void *a, const void *b, void *arg)
{
	return kept_cmp(arg, a, b);
}

SiftProjStream
sift_proj_topk_stream(SiftProjTopK *t)
{
	if (!t->sorted)
	{
		qsort_arg(t->kept, t->n, sizeof(Kept), sort_cmp, t);
		t->sorted = true;
	}
	return (SiftProjStream) {topk_next, t};
}

void
sift_proj_topk_free(SiftProjTopK *t)
{
	for (int i = 0; i < t->n; i++)
		kept_free(&t->kept[i]);
	if (t->kept != NULL)
		pfree(t->kept);
	pfree(t);
}
