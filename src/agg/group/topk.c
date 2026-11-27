/*
 * agg/group/topk.c - the heap of the best K groups (agg/group/topk.h): a binary heap whose top is the worst row held, so that a row is told from the K best by one compare.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#include "agg/group/topk.h"
#include "core/memtrack.h"

/* A held row: its Datums and flags are made once for the slot, and its by-reference values are packed in vbuf, which only grows: a row that replaces the worst allocates nothing once the slot has held a row as large */
typedef struct TopRow
{
	Datum	   *values;
	bool	   *isnull;
	char	   *vbuf;
	uint32		vcap;
} TopRow;

struct SiftGTopK
{
	MemoryContext cx;
	uint32		k;
	uint32		n;
	int			nitems;
	int			item_pos[SIFT_TOPK_MAX_ITEMS];
	int			ncols;
	int16	   *typlen;
	bool	   *typbyval;
	SiftTopKCmp cmp;
	void	   *cmp_arg;
	TopRow	   *rows;
	Size		vbytes;			/* the vcap of every slot: the copies of by-reference values, which take work_mem */
};

SiftGTopK *
sift_gtopk_new(MemoryContext cx, uint32 k, int nitems, const int *item_pos, int ncols, const int16 *typlen, const bool *typbyval, SiftTopKCmp cmp, void *cmp_arg)
{
	SiftGTopK   *t = MemoryContextAllocZero(cx, sizeof(SiftGTopK));

	t->cx = cx;
	t->k = k;
	t->nitems = nitems;
	memcpy(t->item_pos, item_pos, nitems * sizeof(int));
	t->ncols = ncols;
	t->typlen = MemoryContextAlloc(cx, ncols * sizeof(int16));
	memcpy(t->typlen, typlen, ncols * sizeof(int16));
	t->typbyval = MemoryContextAlloc(cx, ncols * sizeof(bool));
	memcpy(t->typbyval, typbyval, ncols * sizeof(bool));
	t->cmp = cmp;
	t->cmp_arg = cmp_arg;
	t->rows = MemoryContextAlloc(cx, Min(k, 1024U) * sizeof(TopRow));
	return t;
}

/* row a sorts after row b: the worse one, which the heap keeps on top */
static int
rows_cmp(const SiftGTopK *t, const TopRow *a, const TopRow *b)
{
	for (int i = 0; i < t->nitems; i++)
	{
		const int	c = t->item_pos[i];
		const int	r = t->cmp(i, a->values[c], a->isnull[c], b->values[c], b->isnull[c], t->cmp_arg);

		if (r != 0)
			return r;
	}
	return 0;
}

static void
sift_up(SiftGTopK *t, uint32 i)
{
	while (i > 0)
	{
		const uint32 parent = (i - 1) / 2;
		TopRow		tmp;

		if (rows_cmp(t, &t->rows[i], &t->rows[parent]) <= 0)
			break;
		tmp = t->rows[i];
		t->rows[i] = t->rows[parent];
		t->rows[parent] = tmp;
		i = parent;
	}
}

static void
sift_down(SiftGTopK *t, uint32 i, uint32 n)
{
	for (;;)
	{
		uint32		worst = i,
					l = 2 * i + 1,
					r = l + 1;
		TopRow		tmp;

		if (l < n && rows_cmp(t, &t->rows[l], &t->rows[worst]) > 0)
			worst = l;
		if (r < n && rows_cmp(t, &t->rows[r], &t->rows[worst]) > 0)
			worst = r;
		if (worst == i)
			return;
		tmp = t->rows[i];
		t->rows[i] = t->rows[worst];
		t->rows[worst] = tmp;
		i = worst;
	}
}

bool
sift_gtopk_wants(SiftGTopK *t, SiftTopKValue value, void *ctx)
{
	if (t->n < t->k)
		return true;
	for (int i = 0; i < t->nitems; i++)
	{
		bool		an;
		const Datum a = value(ctx, i, &an);
		const int	c = t->item_pos[i];
		const int	r = t->cmp(i, a, an, t->rows[0].values[c], t->rows[0].isnull[c], t->cmp_arg);

		if (r != 0)
			return r < 0;
	}
	return false;
}

/* MEM[MEM-AGG-16]: top-k slot arrays and value buffer in the group context for the query (k slots times ncols times 9 bytes plus one power of two buffer per slot within work_mem; a slot is refilled in place, never allocated again per row) */
/* The row (copied) into the slot r */
static void
fill_row(SiftGTopK *t, TopRow *r, const Datum *values, const bool *isnull)
{
	uint64		total = 0;
	uint32		at = 0;

	if (r->values == NULL)
	{
		r->values = MemoryContextAlloc(t->cx, t->ncols * sizeof(Datum));
		r->isnull = MemoryContextAlloc(t->cx, t->ncols * sizeof(bool));
	}
	for (int c = 0; c < t->ncols; c++)
		if (!isnull[c] && !t->typbyval[c])
			total += MAXALIGN(datumGetSize(values[c], false, t->typlen[c]));
	if (total > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a row of an ORDER BY .. LIMIT of the groups is over 1 GB"),
						errdetail("A row held by the top K is copied into one allocation."), errhint("Return fewer or shorter columns.")));
	if (total > r->vcap)
	{
		const uint32 cap = (uint32) Min(Max((uint64) 64, pg_nextpower2_64(total)), (uint64) MaxAllocSize);

		t->vbytes += cap - r->vcap;
		sift_budget_check(t->vbytes, work_mem, "the heap of an ORDER BY .. LIMIT of the groups", "work_mem", "lower the LIMIT");
		r->vbuf = r->vbuf ? repalloc(r->vbuf, cap) : MemoryContextAlloc(t->cx, cap);
		r->vcap = cap;
	}
	for (int c = 0; c < t->ncols; c++)
	{
		r->isnull[c] = isnull[c];
		if (isnull[c])
			r->values[c] = (Datum) 0;
		else if (t->typbyval[c])
			r->values[c] = values[c];
		else
		{
			const Size	size = datumGetSize(values[c], false, t->typlen[c]);

			memcpy(r->vbuf + at, DatumGetPointer(values[c]), size);
			r->values[c] = PointerGetDatum(r->vbuf + at);
			at += MAXALIGN(size);
		}
	}
}

static void
free_row(TopRow *r)
{
	if (r->vbuf)
		pfree(r->vbuf);
	pfree(r->values);
	pfree(r->isnull);
}

void
sift_gtopk_put(SiftGTopK *t, const Datum *values, const bool *isnull)
{
	if (t->n < t->k)
	{
		if (t->n >= Min(t->k, 1024U) && (t->n & (t->n - 1)) == 0)
			/* check-memory: allow the rows kept are at most k, the LIMIT of the query */
			/* check-memory: allow k is at most SIFT_TOPK_MAX_K */
			t->rows = repalloc(t->rows, Min((uint64) t->k, (uint64) t->n * 2) * sizeof(TopRow));
		t->rows[t->n] = (TopRow) {0};
		fill_row(t, &t->rows[t->n], values, isnull);
		sift_up(t, t->n++);
		return;
	}
	fill_row(t, &t->rows[0], values, isnull);
	sift_down(t, 0, t->n);
}

void
sift_gtopk_finish(SiftGTopK *t)
{
	for (uint32 n = t->n; n > 1; n--)
	{
		const TopRow tmp = t->rows[0];

		t->rows[0] = t->rows[n - 1];
		t->rows[n - 1] = tmp;
		sift_down(t, 0, n - 1);
	}
}

bool
sift_gtopk_worst(const SiftGTopK *t, int item, Datum *value, bool *isnull)
{
	if (t->n < t->k)
		return false;
	*value = t->rows[0].values[t->item_pos[item]];
	*isnull = t->rows[0].isnull[t->item_pos[item]];
	return true;
}

uint32
sift_gtopk_count(const SiftGTopK *t)
{
	return t->n;
}

void
sift_gtopk_row(const SiftGTopK *t, uint32 i, const Datum **values, const bool **isnull)
{
	*values = t->rows[i].values;
	*isnull = t->rows[i].isnull;
}

void
sift_gtopk_free(SiftGTopK *t)
{
	for (uint32 i = 0; i < t->n; i++)
		free_row(&t->rows[i]);
	pfree(t->rows);
	pfree(t->typlen);
	pfree(t->typbyval);
	pfree(t);
}
