/*
 * bm25/cscan_keyed.c - SiftScan ordered by leading sort keys, then score: the rounds of a keyed scan.
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "nodes/tidbitmap.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"

#include "bm25/eval.h"
#include "bm25/planner.h"
#include "bm25/cscan_int.h"

/*
 * ORDER BY keys, score DESC: the score only breaks the ties of the keys, so the matches to score are those of
 * the best keys and the ones tied with the last of them. A round passes over the matches in heap order for the
 * kheld best (KeyTop, ties kept), has the ranker score just those through a filter of their entries, and sorts
 * them by keys, score and entry. A consumer that reads past them (a join that drops rows) makes the next round
 * keep more, as the ranker searches again for more hits.
 */
typedef struct KeyTop
{
	SiftScanState *ss;
	binaryheap *heap;			/* KeyRows, the worst keys at the top */
	ItemPointerData *tie;		/* entries of the matches tied with the top that came when the heap was full */
	int64		ntie,
				captie,
				seen;
} KeyTop;

static int
key_compare(SiftScanState *ss, const Datum *a, const bool *an, const Datum *b, const bool *bn)
{
	for (int i = 0; i < ss->nkeys; i++)
	{
		int			c = ApplySortComparator(a[i], an[i], b[i], bn[i], &ss->keysort[i]);

		if (c != 0)
			return c;
	}
	return 0;
}

static int
keyrow_worse(Datum a, Datum b, void *arg)
{
	const KeyRow *x = (const KeyRow *) DatumGetPointer(a),
			   *y = (const KeyRow *) DatumGetPointer(b);

	return key_compare(arg, x->key, x->isnull, y->key, y->isnull);
}

static KeyRow *
new_keyrow(SiftScanState *ss, const ItemPointerData *entry, const Datum *key, const bool *isnull)
{
	KeyRow	   *r = palloc(sizeof(KeyRow) + ss->nkeys * (sizeof(Datum) + sizeof(bool)));

	r->tid = *entry;
	r->key = (Datum *) (r + 1);
	r->isnull = (bool *) (r->key + ss->nkeys);
	for (int i = 0; i < ss->nkeys; i++)
	{
		r->isnull[i] = isnull[i];
		r->key[i] = isnull[i] ? 0 : datumCopy(key[i], ss->keybyval[i], ss->keylen[i]);
	}
	return r;
}

static void
free_keyrow(SiftScanState *ss, KeyRow *r)
{
	for (int i = 0; i < ss->nkeys; i++)
		if (!ss->keybyval[i] && !r->isnull[i])
			pfree(DatumGetPointer(r->key[i]));
	pfree(r);
}

static void
add_tie(KeyTop *t, const ItemPointerData *entry)
{
	if (t->ntie == t->captie)
	{
		t->captie = Max(64, 2 * t->captie);
		t->tie = t->tie ? repalloc_huge(t->tie, t->captie * sizeof(ItemPointerData)) :
			MemoryContextAllocHuge(CurrentMemoryContext, t->captie * sizeof(ItemPointerData));
	}
	t->tie[t->ntie++] = *entry;
}

/* The match of index entry entry with keys key: kept when its keys are among the kheld best so far, or tied with the worst of them */
static void
keytop_add(KeyTop *t, const ItemPointerData *entry, const Datum *key, const bool *isnull)
{
	SiftScanState *ss = t->ss;
	KeyRow	   *worst,
			   *best;
	int			c;

	t->seen++;
	if (binaryheap_size(t->heap) < ss->kheld)
	{
		binaryheap_add(t->heap, PointerGetDatum(new_keyrow(ss, entry, key, isnull)));
		return;
	}
	worst = (KeyRow *) DatumGetPointer(binaryheap_first(t->heap));
	if ((c = key_compare(ss, key, isnull, worst->key, worst->isnull)) > 0)
		return;
	if (c == 0)
	{
		add_tie(t, entry);
		return;
	}
	binaryheap_replace_first(t->heap, PointerGetDatum(new_keyrow(ss, entry, key, isnull)));
	best = (KeyRow *) DatumGetPointer(binaryheap_first(t->heap));
	/* the ties were of the keys that left the top: they stay with the keys that took its place only when those are the same */
	if (key_compare(ss, worst->key, worst->isnull, best->key, best->isnull) == 0)
		add_tie(t, &worst->tid);
	else
		t->ntie = 0;
	free_keyrow(ss, worst);
}

/* Passes over the matches in heap order that pass the quals */
static void
keyed_collect(SiftScanState *ss, KeyTop *t)
{
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;
	Datum	   *key = palloc(ss->nkeys * sizeof(Datum));
	bool	   *isnull = palloc(ss->nkeys * sizeof(bool));
	ItemPointerData entry;

	ss->it = sift_iter_begin(ss->plan);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		ResetExprContext(econtext);
		if (next_row(ss, &entry) == NULL)
			break;
		for (int i = 0; i < ss->nkeys; i++)
			key[i] = ExecEvalExprSwitchContext(ss->keyexpr[i], econtext, &isnull[i]);
		keytop_add(t, &entry, key, isnull);
	}
	reset_iter(ss);
}

static int
entry_order(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) &(*(KeyRow *const *) a)->tid, (ItemPointer) &(*(KeyRow *const *) b)->tid);
}

/* The rows the pass kept, with the ties given the keys of the worst, in order of entry */
static KeyRow **
keytop_rows(KeyTop *t, int64 *nrows)
{
	int64		nheap = binaryheap_size(t->heap);
	KeyRow	  **rows = palloc((nheap + t->ntie) * sizeof(KeyRow *));
	KeyRow	   *tied = palloc(t->ntie * sizeof(KeyRow));

	for (int64 i = 0; i < nheap; i++)
		rows[i] = (KeyRow *) DatumGetPointer(binaryheap_get_node(t->heap, i));
	for (int64 i = 0; i < t->ntie; i++)
	{
		const KeyRow *worst = rows[0];

		tied[i] = (KeyRow) {.tid = t->tie[i], .key = worst->key, .isnull = worst->isnull};
		rows[nheap + i] = &tied[i];
	}
	*nrows = nheap + t->ntie;
	qsort(rows, *nrows, sizeof(KeyRow *), entry_order);
	return rows;
}

static int
entry_find(const void *entry, const void *row)
{
	return ItemPointerCompare((ItemPointer) entry, (ItemPointer) &(*(KeyRow *const *) row)->tid);
}

/* Scores the rows (in order of entry): the ranker looks at nothing but their tuples */
static void
keyed_score(SiftScanState *ss, KeyRow **rows, int64 nrows)
{
	SiftFilter *filter = sift_filter_create(CurrentMemoryContext);
	ItemPointerData tid;
	float4		score;
	int64		nscored = 0;

	for (int64 i = 0; i < nrows;)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&rows[i]->tid);
		uint64		mask[SIFT_OFFMASK_WORDS] = {0};

		for (; i < nrows && ItemPointerGetBlockNumber(&rows[i]->tid) == blk; i++)
		{
			int			off = ItemPointerGetOffsetNumber(&rows[i]->tid) - 1;

			mask[off / 64] |= UINT64CONST(1) << (off % 64);
		}
		sift_filter_add_page(filter, blk, mask);
	}
	sift_plan_set_filter(ss->plan, filter);
	ss->rank = sift_rank_begin(ss->plan, 0, scan_verify, ss);
	while (sift_rank_next(ss->rank, &tid, &score))
	{
		KeyRow	  **row = bsearch(&tid, rows, nrows, sizeof(KeyRow *), entry_find);

		if (row)
		{
			(*row)->score = score;
			nscored++;
		}
	}
	sift_rank_end(ss->rank);
	ss->rank = NULL;
	SIFT_INVARIANT(nscored == nrows, "sift: the ranker scored " INT64_FORMAT " of the " INT64_FORMAT " matches the keys kept", nscored,
				   nrows);
}

static int
keyrow_order(const void *a, const void *b, void *arg)
{
	const KeyRow *x = *(KeyRow *const *) a,
			   *y = *(KeyRow *const *) b;
	int			c = key_compare(arg, x->key, x->isnull, y->key, y->isnull);

	if (c != 0)
		return c;
	if (x->score != y->score)
		return x->score > y->score ? -1 : 1;
	return ItemPointerCompare((ItemPointer) &x->tid, (ItemPointer) &y->tid);
}

void
keyed_search(SiftScanState *ss)
{
	KeyTop		t = {.ss = ss};
	MemoryContext old;

	if (ss->kcx)
		MemoryContextDelete(ss->kcx);
	ss->kcx = AllocSetContextCreate(ss->cxt, "sift keyed", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(ss->kcx);
	t.heap = binaryheap_allocate(ss->kheld, keyrow_worse, ss);
	keyed_collect(ss, &t);
	ss->krows = keytop_rows(&t, &ss->nkrows);
	ss->kcomplete = t.seen <= ss->kheld;
	ss->kpasses++;
	if (ss->nkrows > 0)
		keyed_score(ss, ss->krows, ss->nkrows);
	qsort_arg(ss->krows, ss->nkrows, sizeof(KeyRow *), keyrow_order, ss);
	MemoryContextSwitchTo(old);
}

TupleTableSlot *
keyed_next(SiftScanState *ss)
{
	for (;;)
	{
		while (ss->kpos < ss->nkrows)
		{
			KeyRow	   *r = ss->krows[ss->kpos++];
			ItemPointerData tid = r->tid;

			if (sift_vis_fetch(&ss->vis, &tid, ss->heapslot))
				return fill_scan_slot(ss, r->score);
		}
		if (ss->kcomplete)
			return NULL;
		/* more than it holds, which a big tie of the last keys can exceed several times over */
		ss->kheld = Min(Max(sift_rank_next_k(ss->kheld), ss->nkrows + 1), PG_INT32_MAX);
		keyed_search(ss);
	}
}
