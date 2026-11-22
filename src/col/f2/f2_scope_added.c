/*
 * col/f2/f2_scope_added.c - the newest rows of a key among those the summary lacks (a slab, a part it does not name) and the cells of such rows: what a read of the newest rows of the key
 * needs to put them among the rows of the ring (f2_scope_over.c keeps the rows and where their cells are).
 */
/* check-memory: allow-allocs the rows asked for are those of one key, at most ADDED_TOP_MAX */
#include "postgres.h"

#include "col/f2/f2_scope_int.h"
#include "col/f2/rowbuf.h"

#define ADDED_TOP_MAX 4096

static void
heap_offer(int64 *h, uint32 *nh, uint32 cap, int64 t)
{
	uint32		i;

	if (*nh == cap)
	{
		if (t <= h[0])
			return;
		i = 0;
		for (;;)
		{
			uint32		c = 2 * i + 1;

			if (c >= *nh)
				break;
			if (c + 1 < *nh && h[c + 1] < h[c])
				c++;
			if (h[c] >= t)
				break;
			h[i] = h[c];
			i = c;
		}
		h[i] = t;
		return;
	}
	i = (*nh)++;
	while (i > 0 && h[(i - 1) / 2] > t)
	{
		h[i] = h[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	h[i] = t;
}

static int
newest_first(const void *a, const void *b)
{
	const SiftF2ScopeAdded *x = a,
			   *y = b;

	return x->time != y->time ? (x->time > y->time ? -1 : 1) : 0;
}

uint32
sift_f2_scope_added_top(const SiftF2Scope *sc, int64 key, uint32 n, SiftF2ScopeAdded **out)
{
	SiftF2ScopeParts *p = sc->entity.arg;
	const struct SiftF2ScopeRowSet *a = p->added;
	uint32		nruns,
				nh = 0,
				found = 0;
	uint32	   *runs;
	int64	   *heap;
	int64		cut;

	*out = NULL;
	if (a == NULL || a->time == NULL || n == 0)
		return 0;
	runs = sift_f2s_key_runs(p, (struct SiftF2ScopeRowSet *) a, key, &nruns);
	heap = palloc(n * sizeof(int64));
	for (uint32 r = 0; r < nruns; r++)
		for (uint32 i = a->run_first[runs[r]]; i < a->run_first[runs[r] + 1]; i++)
			if (!a->time_null[i])
				heap_offer(heap, &nh, n, a->time[i]);
	cut = nh < n ? PG_INT64_MIN : heap[0];
	*out = palloc(Min(ADDED_TOP_MAX, Max(nh, 1) + 16) * sizeof(SiftF2ScopeAdded));
	for (uint32 r = 0; r < nruns; r++)
		for (uint32 i = a->run_first[runs[r]]; i < a->run_first[runs[r] + 1]; i++)
			if (!a->time_null[i] && a->time[i] >= cut)
			{
				if (found == Min(ADDED_TOP_MAX, Max(nh, 1) + 16))
					return UINT32_MAX;
				(*out)[found++] = (SiftF2ScopeAdded) {a->time[i], a->src[i], a->g[i], a->row[i]};
			}
	pfree(runs);
	pfree(heap);
	qsort(*out, found, sizeof(SiftF2ScopeAdded), newest_first);
	return found;
}

static int
by_source(const void *x, const void *y)
{
	const SiftF2ScopeAdded *a = *(const SiftF2ScopeAdded *const *) x,
			   *b = *(const SiftF2ScopeAdded *const *) y;

	if (a->src != b->src)
		return a->src < b->src ? -1 : 1;
	return a->g < b->g ? -1 : a->g > b->g;
}

static void
slab_cells(const SiftRowBuf *rb, const SiftF2ScopeAdded *r, const int *leaves, int nleaves, SiftF2ScopeCell *cells)
{
	for (int j = 0; j < nleaves; j++)
	{
		const SiftCell c = sift_rowbuf_cell(rb, leaves[j], (int) r->row);
		char	   *copy = c.s != NULL && !c.isnull ? memcpy(palloc(c.slen + 1), c.s, c.slen) : NULL;

		if (copy != NULL)
			copy[c.slen] = '\0';
		cells[j] = (SiftF2ScopeCell) {.i = c.i,.f = c.f,.s = copy,.len = c.slen,.isnull = c.isnull,.tag = c.tag};
	}
}

void
sift_f2_scope_added_cells(SiftF2Scope *sc, const SiftF2ScopeAdded *rows, int nrows, const int *leaves, int nleaves, SiftF2ScopeCell *cells)
{
	SiftF2ScopeParts *p = sc->entity.arg;
	const SiftF2ScopeAdded **order = palloc(Max(nrows, 1) * sizeof(*order));
	SiftScopeNewest *as_rows = palloc(Max(nrows, 1) * sizeof(SiftScopeNewest));
	const SiftScopeNewest **picked = palloc(Max(nrows, 1) * sizeof(*picked));
	MemoryContext out = CurrentMemoryContext,
				cx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope added cells");

	for (int k = 0; k < nrows; k++)
	{
		order[k] = &rows[k];
		as_rows[k] = (SiftScopeNewest) {rows[k].time, 0, rows[k].row};
	}
	qsort(order, nrows, sizeof(*order), by_source);
	for (int k = 0, e; k < nrows; k = e)
	{
		for (e = k; e < nrows && order[e]->src == order[k]->src && order[e]->g == order[k]->g; e++)
			;
		if (!(order[k]->src & SIFT_F2S_SRC_PART))
		{
			for (int q = k; q < e; q++)
				slab_cells(p->added->bufs[order[q]->src], order[q], leaves, nleaves, cells + (order[q] - rows) * nleaves);
			continue;
		}
		for (int q = k; q < e; q++)
			picked[q - k] = &as_rows[order[q] - rows];
		{
			MemoryContext old = MemoryContextSwitchTo(cx);

			sift_f2s_granule_cells(p, &p->s[order[k]->src & ~SIFT_F2S_SRC_PART], order[k]->g, picked, e - k, as_rows, leaves, nleaves, cells, out);
			MemoryContextSwitchTo(old);
		}
		MemoryContextReset(cx);
	}
	MemoryContextDelete(cx);
}
