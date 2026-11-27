/*
 * agg/group/topk.c - the heap of the best K groups (agg/group/topk.h): a binary heap whose top is the worst row held, so that a row is told from the K best by one compare. When the copies of the
 * rows held would pass work_mem, the rows move to a sort bounded by K, which writes runs to disk as a Sort does: from then on every row offered goes to it.
 */
#include "postgres.h"

#include "executor/tuptable.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"

#include "agg/group/topk.h"

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
	uint32		at;
	int			nitems;
	SiftV6TopKItem items[SIFT_V6_MAX_TOPK_ITEMS];
	SortSupportData ssup[SIFT_V6_MAX_TOPK_ITEMS];
	TupleDesc	desc;
	TopRow	   *rows;
	Size		vbytes;			/* the vcap of every slot: the copies of by-reference values, which take work_mem */
	Tuplesortstate *sort;		/* the rows once their copies passed work_mem */
	TupleTableSlot *slot;
	uint64		sorted;
};

SiftGTopK *
sift_gtopk_new(MemoryContext cx, uint32 k, TupleDesc desc, int nitems, const SiftV6TopKItem *items)
{
	SiftGTopK   *t = MemoryContextAllocZero(cx, sizeof(SiftGTopK));

	t->cx = cx;
	t->k = k;
	t->desc = desc;
	t->nitems = nitems;
	memcpy(t->items, items, nitems * sizeof(SiftV6TopKItem));
	for (int i = 0; i < nitems; i++)
	{
		t->ssup[i] = (SortSupportData) {.ssup_cxt = cx,.ssup_collation = items[i].collid,.ssup_nulls_first = items[i].nulls_first,.ssup_attno = items[i].pos + 1};
		PrepareSortSupportFromOrderingOp(items[i].sortop, &t->ssup[i]);
	}
	t->rows = MemoryContextAlloc(cx, Min(k, 1024U) * sizeof(TopRow));
	return t;
}

static int
item_cmp(const SiftGTopK *t, int i, Datum a, bool a_null, Datum b, bool b_null)
{
	return ApplySortComparator(a, a_null, b, b_null, (SortSupport) &t->ssup[i]);
}

/* row a sorts after row b: the worse one, which the heap keeps on top */
static int
rows_cmp(const SiftGTopK *t, const TopRow *a, const TopRow *b)
{
	for (int i = 0; i < t->nitems; i++)
	{
		const int	c = t->items[i].pos;
		const int	r = item_cmp(t, i, a->values[c], a->isnull[c], b->values[c], b->isnull[c]);

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
	if (t->sort != NULL || t->n < t->k)
		return true;
	for (int i = 0; i < t->nitems; i++)
	{
		bool		an;
		const Datum a = value(ctx, i, &an);
		const int	c = t->items[i].pos;
		const int	r = item_cmp(t, i, a, an, t->rows[0].values[c], t->rows[0].isnull[c]);

		if (r != 0)
			return r < 0;
	}
	return false;
}

/* The bytes the by-reference values of a row take packed */
static uint64
row_bytes(const SiftGTopK *t, const Datum *values, const bool *isnull)
{
	uint64		total = 0;

	for (int c = 0; c < t->desc->natts; c++)
		if (!isnull[c] && !TupleDescAttr(t->desc, c)->attbyval)
			total += MAXALIGN(datumGetSize(values[c], false, TupleDescAttr(t->desc, c)->attlen));
	if (total > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a row of an ORDER BY .. LIMIT of the groups is over 1 GB"),
						errdetail("A row held by the top K is copied into one allocation."), errhint("Return fewer or shorter columns.")));
	return total;
}

static uint32
slot_cap(uint64 total)
{
	return (uint32) Min(Max((uint64) 64, pg_nextpower2_64(total)), (uint64) MaxAllocSize);
}

/* MEM[MEM-AGG-16]: top-k slot arrays and value buffer in the group context for the query (k slots times ncols times 9 bytes plus one power of two buffer per slot within work_mem, past which the rows go to a tuplesort; a slot is refilled in place, never allocated again per row) */
/* The row (copied) into the slot r, whose buffer holds total bytes */
static void
fill_row(SiftGTopK *t, TopRow *r, const Datum *values, const bool *isnull, uint64 total)
{
	const int	ncols = t->desc->natts;
	uint32		at = 0;

	if (r->values == NULL)
	{
		r->values = MemoryContextAlloc(t->cx, ncols * sizeof(Datum));
		r->isnull = MemoryContextAlloc(t->cx, ncols * sizeof(bool));
	}
	if (total > r->vcap)
	{
		const uint32 cap = slot_cap(total);

		t->vbytes += cap - r->vcap;
		r->vbuf = r->vbuf ? repalloc(r->vbuf, cap) : MemoryContextAlloc(t->cx, cap);
		r->vcap = cap;
	}
	for (int c = 0; c < ncols; c++)
	{
		const Form_pg_attribute att = TupleDescAttr(t->desc, c);

		r->isnull[c] = isnull[c];
		if (isnull[c])
			r->values[c] = (Datum) 0;
		else if (att->attbyval)
			r->values[c] = values[c];
		else
		{
			const Size	size = datumGetSize(values[c], false, att->attlen);

			memcpy(r->vbuf + at, DatumGetPointer(values[c]), size);
			r->values[c] = PointerGetDatum(r->vbuf + at);
			at += MAXALIGN(size);
		}
	}
}

static void
sort_put(SiftGTopK *t, const Datum *values, const bool *isnull)
{
	ExecClearTuple(t->slot);
	memcpy(t->slot->tts_values, values, t->desc->natts * sizeof(Datum));
	memcpy(t->slot->tts_isnull, isnull, t->desc->natts * sizeof(bool));
	ExecStoreVirtualTuple(t->slot);
	tuplesort_puttupleslot(t->sort, t->slot);
	t->sorted++;
}

/* The rows held go to a sort bounded by K, and the heap goes with its copies */
static void
spill(SiftGTopK *t)
{
	MemoryContext old = MemoryContextSwitchTo(t->cx);
	AttrNumber	att[SIFT_V6_MAX_TOPK_ITEMS];
	Oid			op[SIFT_V6_MAX_TOPK_ITEMS],
				coll[SIFT_V6_MAX_TOPK_ITEMS];
	bool		nulls_first[SIFT_V6_MAX_TOPK_ITEMS];

	for (int i = 0; i < t->nitems; i++)
	{
		att[i] = (AttrNumber) (t->items[i].pos + 1);
		op[i] = t->items[i].sortop;
		coll[i] = t->items[i].collid;
		nulls_first[i] = t->items[i].nulls_first;
	}
	t->sort = tuplesort_begin_heap(t->desc, t->nitems, att, op, coll, nulls_first, work_mem, NULL, TUPLESORT_ALLOWBOUNDED);
	tuplesort_set_bound(t->sort, t->k);
	t->slot = MakeSingleTupleTableSlot(t->desc, &TTSOpsMinimalTuple);
	for (uint32 i = 0; i < t->n; i++)
	{
		sort_put(t, t->rows[i].values, t->rows[i].isnull);
		if (t->rows[i].vbuf)
			pfree(t->rows[i].vbuf);
		pfree(t->rows[i].values);
		pfree(t->rows[i].isnull);
	}
	pfree(t->rows);
	t->rows = NULL;
	t->n = 0;
	t->vbytes = 0;
	MemoryContextSwitchTo(old);
}

void
sift_gtopk_put(SiftGTopK *t, const Datum *values, const bool *isnull)
{
	const uint64 total = row_bytes(t, values, isnull);
	const uint32 vcap = t->sort == NULL && t->n == t->k ? t->rows[0].vcap : 0;

	if (t->sort == NULL && total > vcap && (t->vbytes + slot_cap(total) - vcap) / 1024 > (Size) work_mem)
		spill(t);
	if (t->sort != NULL)
	{
		sort_put(t, values, isnull);
		return;
	}
	if (t->n < t->k)
	{
		if (t->n >= Min(t->k, 1024U) && (t->n & (t->n - 1)) == 0)
			/* check-memory: allow the rows kept are at most k, the LIMIT of the query */
			/* check-memory: allow k is at most SIFT_TOPK_MAX_K */
			t->rows = repalloc(t->rows, Min((uint64) t->k, (uint64) t->n * 2) * sizeof(TopRow));
		t->rows[t->n] = (TopRow) {0};
		fill_row(t, &t->rows[t->n], values, isnull, total);
		sift_up(t, t->n++);
		return;
	}
	fill_row(t, &t->rows[0], values, isnull, total);
	sift_down(t, 0, t->n);
}

void
sift_gtopk_finish(SiftGTopK *t)
{
	if (t->sort != NULL)
	{
		tuplesort_performsort(t->sort);
		return;
	}
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
	if (t->sort != NULL || t->n < t->k)
		return false;
	*value = t->rows[0].values[t->items[item].pos];
	*isnull = t->rows[0].isnull[t->items[item].pos];
	return true;
}

uint32
sift_gtopk_count(const SiftGTopK *t)
{
	return t->sort != NULL ? (uint32) Min(t->sorted, (uint64) t->k) : t->n;
}

bool
sift_gtopk_next(SiftGTopK *t, const Datum **values, const bool **isnull)
{
	if (t->at >= sift_gtopk_count(t))
		return false;
	t->at++;
	if (t->sort == NULL)
	{
		*values = t->rows[t->at - 1].values;
		*isnull = t->rows[t->at - 1].isnull;
		return true;
	}
	tuplesort_gettupleslot(t->sort, true, false, t->slot, NULL);
	slot_getallattrs(t->slot);
	*values = t->slot->tts_values;
	*isnull = t->slot->tts_isnull;
	return true;
}

void
sift_gtopk_end(SiftGTopK *t)
{
	if (t->sort == NULL)
		return;
	tuplesort_end(t->sort);
	ExecDropSingleTupleTableSlot(t->slot);
	t->sort = NULL;
}
