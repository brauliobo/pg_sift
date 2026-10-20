/*
 * sift_cscan.c - executor side of SiftScan (heap order or ranked) and
 * SiftCount (DESIGN.md (f), (e)).
 *
 * The scan tuple has the layout of custom_scan_tlist: needed heap columns,
 * then sift.score / sift.max_score values the scan computes; upper nodes and
 * the residual quals read it through INDEX_VAR. Visibility, residual quals and
 * the phrase text come from verify() callbacks into the heap, so the ranker
 * only verifies candidates that would enter the top-k.
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
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"

#include "sift_eval.h"
#include "sift_planner.h"

typedef enum SiftColKind
{
	COL_VAR,					/* heap column attno (0: whole row, < 0: system column) */
	COL_SCORE,
	COL_MAX_SCORE,
	COL_COUNT
} SiftColKind;

typedef struct SiftCol
{
	SiftColKind kind;
	int			attno;
} SiftCol;

typedef struct SiftTotals
{
	int64		sb_scanned;
	int64		blocks_scored;
	int64		blocks_skipped;
	int64		candidates;
	int64		verified;
	int64		heap_checks;
	int64		heap_pages;
	int64		filter_sbs;			/* pushdown: what the bitmaps kept, summed over the scans */
	int64		filter_exact;
	int64		filter_lossy;
} SiftTotals;

/* Heap pages of an unordered scan read ahead, and the gap from the previous page that is worth a read of its own */
#define PREFETCH_PAGES	16
#define PREFETCH_GAP	8

/* A hint larger than this makes the ranker keep more hits than it is likely to hand out: it grows its search past it */
#define LIMIT_HINT_MAX	(1 << 20)

/* The rows a keyed scan keeps in its first round when it has no hint */
#define KEYED_INITIAL_ROWS	1024

/*
 * A match of a keyed scan that is among the rows of the best leading keys: its index entry, its keys (those
 * of the boundary row for the rows tied with it) and the score the ranker gives it
 */
typedef struct KeyRow
{
	ItemPointerData tid;
	float4		score;
	Datum	   *key;
	bool	   *isnull;
} KeyRow;

typedef struct SiftScanState
{
	CustomScanState css;
	SiftScanMode mode;
	Oid			indexoid;
	Oid			heapoid;
	int64		k;
	List	   *queries;			/* ExprState */
	ExprState  *quals;				/* residual quals over the scan tuple */
	ExprState  *cuts;				/* cutoffs of the score, over the scan tuple of a ranked row */
	Relation	heap;
	bool		heap_opened;
	Relation	index;
	IndexInfo  *iinfo;
	TupleTableSlot *heapslot;
	int			ncols;
	SiftCol    *col;
	MemoryContext cxt;				/* everything of one scan */
	MemoryContext tmpcx;			/* one candidate's text */
	bool		started;
	bool		empty;				/* a query is NULL: no rows */
	bool		done;				/* count: row returned */
	bool		need_heap;
	SiftIndexState *st;
	SiftPlan   *plan;
	PlanState  *bitmap;				/* ranked pushdown: bitmap of the tuples the other quals keep */
	double		gate,				/* the bitmap is built when the query matches between gate and gate_to documents */
				gate_to;
	SiftFilter *filter;
	SiftMatchIter *it;
	SiftRanker *rank;
	SiftVis		vis;
	bool		vis_open;
	float4		max_score;
	SiftPageMatch pm;
	SiftPageMatch ahead[PREFETCH_PAGES];	/* matches read from the iterator, whose heap pages are being read */
	int			ahead_pos;
	int			ahead_n;
	BlockNumber ahead_blk;				/* last page read from the iterator */
	bool		ahead_done;
	int			wd;
	uint64		mask;
	uint32		vm_sb;
	uint64		allvis[SIFT_PAGEMASK_WORDS];
	int64		nsearches;
	SiftTotals	tot;
	SiftPar		par;				/* parallel-aware plan run with workers: par.sh is the shared state */
	/* keyed scan: by the leading sort keys, then by score */
	int			nkeys;				/* 0: by score alone */
	ExprState **keyexpr;
	SortSupportData *keysort;
	int16	   *keylen;
	bool	   *keybyval;
	MemoryContext kcx;				/* the rows of the current round */
	KeyRow	  **krows;				/* in order of keys, score and entry */
	int64		nkrows,
				kpos,				/* rows handed up */
				kheld,				/* the best keys' rows the current round keeps, besides the ties of the last */
				kpasses;			/* rounds so far */
	bool		kcomplete;			/* the round holds every match */
} SiftScanState;

/* ---------- scan tuple ---------- */

static void
init_cols(SiftScanState *ss, CustomScan *cs)
{
	int			i = 0;

	ss->ncols = list_length(cs->custom_scan_tlist);
	ss->col = palloc0(Max(ss->ncols, 1) * sizeof(SiftCol));
	foreach_node(TargetEntry, te, cs->custom_scan_tlist)
	{
		Node	   *e = (Node *) te->expr;
		bool		is_max;

		if (IsA(e, Var))
			ss->col[i] = (SiftCol) {COL_VAR, ((Var *) e)->varattno};
		else if (IsA(e, Aggref))
			ss->col[i].kind = COL_COUNT;
		else if (sift_is_score_expr(e, cs->scan.scanrelid, &is_max))
			ss->col[i].kind = is_max ? COL_MAX_SCORE : COL_SCORE;
		else
			elog(ERROR, "sift: unexpected scan tuple column");
		i++;
	}
}

/* The scan tuple of the heap tuple in heapslot; scores are those of the ranked hit */
static TupleTableSlot *
fill_scan_slot(SiftScanState *ss, float4 score)
{
	TupleTableSlot *slot = ss->css.ss.ss_ScanTupleSlot;

	ExecClearTuple(slot);
	for (int i = 0; i < ss->ncols; i++)
	{
		const SiftCol *c = &ss->col[i];

		slot->tts_isnull[i] = false;
		switch (c->kind)
		{
			case COL_VAR:
				if (c->attno > 0)
					slot->tts_values[i] = slot_getattr(ss->heapslot, c->attno, &slot->tts_isnull[i]);
				else if (c->attno == 0)
					slot->tts_values[i] = ExecFetchSlotHeapTupleDatum(ss->heapslot);
				else
					slot->tts_values[i] = slot_getsysattr(ss->heapslot, c->attno, &slot->tts_isnull[i]);
				break;
			case COL_SCORE:
				slot->tts_values[i] = Float4GetDatum(score);
				break;
			case COL_MAX_SCORE:
				slot->tts_values[i] = Float4GetDatum(ss->max_score);
				break;
			case COL_COUNT:
				break;
		}
	}
	return ExecStoreVirtualTuple(slot);
}

/* The indexed text of the tuple in heapslot, NULL when the index key is NULL */
static text *
index_text(SiftScanState *ss)
{
	EState	   *estate = ss->css.ss.ps.state;
	Datum		value;
	bool		isnull;

	ResetPerTupleExprContext(estate);
	GetPerTupleExprContext(estate)->ecxt_scantuple = ss->heapslot;
	FormIndexDatum(ss->iinfo, ss->heapslot, estate, &value, &isnull);
	return isnull ? NULL : DatumGetTextPP(value);
}

/* Visible, passes the residual quals, and with want_doc yields the indexed text */
static bool
scan_verify(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	SiftScanState *ss = arg;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	if (!sift_vis_fetch(&ss->vis, tid, ss->heapslot))
		return false;
	if (ss->quals)
	{
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = fill_scan_slot(ss, 0);
		if (!ExecQual(ss->quals, econtext))
			return false;
	}
	return !want_doc || (*doc = index_text(ss)) != NULL;
}

/* ---------- scan lifecycle ---------- */

static void
add_stats(SiftScanState *ss)
{
	const SiftPlanStats *s;

	if (ss->filter)
	{
		ss->tot.filter_sbs += ss->filter->nsb;
		ss->tot.filter_exact += ss->filter->exact_pages;
		ss->tot.filter_lossy += ss->filter->lossy_pages;
	}
	if (ss->plan == NULL)
		return;
	s = sift_plan_stats(ss->plan);
	ss->tot.sb_scanned += s->sb_scanned;
	ss->tot.blocks_scored += s->blocks_scored;
	ss->tot.blocks_skipped += s->blocks_skipped;
	ss->tot.candidates += s->candidates;
	ss->tot.verified += s->verified;
	if (ss->vis_open)
	{
		ss->tot.heap_checks += ss->vis.heap_checks;
		ss->tot.heap_pages += ss->vis.heap_pages;
	}
}

/* Ends the heap-order iteration and the pages read ahead of it */
static void
reset_iter(SiftScanState *ss)
{
	if (ss->it)
		sift_iter_end(ss->it);
	ss->it = NULL;
	ss->ahead_pos = ss->ahead_n = 0;
	ss->ahead_blk = 0;
	ss->ahead_done = false;
	ss->wd = SIFT_OFFMASK_WORDS;
	ss->mask = 0;
	ss->vm_sb = UINT32_MAX;
}

static void
close_scan(SiftScanState *ss)
{
	add_stats(ss);
	if (ss->rank)
		sift_rank_end(ss->rank);
	reset_iter(ss);
	if (ss->vis_open)
		sift_vis_end(&ss->vis);
	if (ss->st)
		sift_state_close(ss->st);
	ss->rank = NULL;
	ss->st = NULL;
	ss->plan = NULL;
	ss->filter = NULL;
	ss->vis_open = false;
	MemoryContextReset(ss->cxt);
	ss->started = ss->done = ss->empty = false;
	ss->kcx = NULL;
	ss->krows = NULL;
	ss->nkrows = ss->kpos = 0;
}

/* The plan binds from the leader's export (shared), or reads the index itself; NULL: a query is NULL, no rows */
static SiftPlan *
bind_queries(SiftScanState *ss, SiftIndexState *st, const void *shared)
{
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;
	int			n = list_length(ss->queries),
				i = 0,
				flags = ss->mode == SIFT_MODE_RANKED ? SIFT_BIND_SCORE : 0;
	SiftQuery **queries = palloc(n * sizeof(SiftQuery *));

	foreach_ptr(ExprState, es, ss->queries)
	{
		bool		isnull;
		Datum		d = ExecEvalExprSwitchContext(es, econtext, &isnull);
		text	   *q;

		if (isnull)
			return NULL;
		q = DatumGetTextPP(d);
		queries[i++] = sift_state_query(st, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
	}
	return shared ? sift_plan_bind_shared(st, queries, n, flags, shared) : sift_plan_bind(st, queries, n, flags);
}

/* Parallel ranked scans score from the one bind the leader made, whoever runs them */
static const void *
shared_bind(SiftScanState *ss)
{
	Size		len;

	return ss->par.sh && ss->mode == SIFT_MODE_RANKED ? sift_par_get_bind(&ss->par, &len) : NULL;
}

static void
bind_scan(SiftScanState *ss)
{
	const void *shared = shared_bind(ss);

	ss->st = shared ? sift_state_open_dir(ss->index, ss->cxt, sift_shared_dir(shared)) :
		sift_state_open(ss->index, ss->cxt);
	ss->plan = bind_queries(ss, ss->st, shared);
	ss->empty = ss->plan == NULL;
}

/*
 * The bitmap of the tuples the other quals keep becomes the masks the ranker restricts itself to. Its
 * node runs in the executor's context like any child; no tuple in it, no rows.
 */
static void
build_filter(SiftScanState *ss)
{
	EState	   *estate = ss->css.ss.ps.state;
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	TIDBitmap  *tbm = (TIDBitmap *) MultiExecProcNode(ss->bitmap);

	MemoryContextSwitchTo(old);
	ss->filter = sift_filter_build(tbm, (Size) work_mem * 1024, ss->cxt);
	tbm_free(tbm);
	ss->empty = sift_filter_empty(ss->filter);
}

/* A pushdown of a plan made without its query (a generic plan) waits for the bound query to say how many documents it matches */
static bool
gated(const SiftScanState *ss)
{
	return ss->gate > 0 || !isinf(ss->gate_to);
}

static bool
in_gate(const SiftScanState *ss, double docs)
{
	return docs >= ss->gate && docs <= ss->gate_to;
}

static void keyed_search(SiftScanState *ss);

static void
start_scan(SiftScanState *ss)
{
	EState	   *estate = ss->css.ss.ps.state;
	MemoryContext old;

	ss->started = true;
	ss->nsearches++;
	if (ss->bitmap && !gated(ss))
		build_filter(ss);
	old = MemoryContextSwitchTo(ss->cxt);
	if (!ss->empty)
		bind_scan(ss);
	if (ss->bitmap && gated(ss) && !ss->empty && in_gate(ss, sift_plan_docs(ss->plan)))
		build_filter(ss);
	if (!ss->empty)
	{
		sift_vis_begin(&ss->vis, ss->heap, estate->es_snapshot, ss->index, sift_plan_dir_version(ss->plan));
		ss->vis_open = true;
		ss->need_heap = ss->ncols > 0 || ss->quals != NULL || sift_plan_recheck_needed(ss->plan);
		if (ss->mode == SIFT_MODE_RANKED && ss->nkeys > 0)
		{
			/* a row more than the limit: an incremental sort above reads the row after its last group to end it */
			ss->kheld = ss->k > 0 ? Min(ss->k + 1, LIMIT_HINT_MAX) : KEYED_INITIAL_ROWS;
			keyed_search(ss);
		}
		else if (ss->mode == SIFT_MODE_RANKED)
		{
			if (ss->filter)
				sift_plan_set_filter(ss->plan, ss->filter);
			ss->rank = sift_rank_begin(ss->plan, ss->k, scan_verify, ss);
			if (ss->par.sh)
				sift_rank_parallel(ss->rank, &ss->par);
			for (int i = 0; i < ss->ncols; i++)
				if (ss->col[i].kind == COL_MAX_SCORE)
					ss->max_score = sift_rank_max_score(ss->rank);
		}
		else if (ss->mode == SIFT_MODE_UNORDERED)
		{
			ss->it = sift_iter_begin(ss->plan);
			if (ss->par.sh)
				sift_iter_ranges(ss->it, sift_par_next_range, &ss->par);
		}
	}
	MemoryContextSwitchTo(old);
}

/* ---------- row sources ---------- */

/* Next heap-order match, from the pages read ahead when the scan returns tuples */
static bool
next_match(SiftScanState *ss)
{
	/* only a heap has pages at the block numbers of its TIDs (those of another table access method are not blocks of its file) */
	if (!ss->need_heap || !sift_prefetch || ss->heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		return sift_iter_next(ss->it, &ss->pm);
	while (ss->ahead_n < PREFETCH_PAGES && !ss->ahead_done)
	{
		SiftPageMatch *pm = &ss->ahead[(ss->ahead_pos + ss->ahead_n) % PREFETCH_PAGES];

		if (!(ss->ahead_done = !sift_iter_next(ss->it, pm)))
		{
			/* the kernel reads ahead by itself while the matching pages are close */
			if (pm->blk - ss->ahead_blk > PREFETCH_GAP)
				PrefetchBuffer(ss->heap, MAIN_FORKNUM, pm->blk);
			ss->ahead_blk = pm->blk;
			ss->ahead_n++;
		}
	}
	if (ss->ahead_n == 0)
		return false;
	ss->pm = ss->ahead[ss->ahead_pos];
	ss->ahead_pos = (ss->ahead_pos + 1) % PREFETCH_PAGES;
	ss->ahead_n--;
	return true;
}

/* Next heap-order page; its candidates are reduced to the visible ones when no tuple is needed */
static bool
load_page(SiftScanState *ss)
{
	if (!next_match(ss))
		return false;
	ss->wd = 0;
	ss->mask = 0;
	if (!ss->need_heap)
	{
		uint32		sb = sift_block_sb(ss->pm.blk),
					page = ss->pm.blk & (SIFT_SB_PAGES - 1);

		if (sb != ss->vm_sb)
		{
			ss->vm_sb = sb;
			sift_vis_sb_mask(&ss->vis, sb, ss->allvis);
		}
		if (!(ss->allvis[page / 64] >> (page % 64) & 1))
			sift_vis_filter_page(&ss->vis, ss->pm.blk, ss->pm.offmask);
	}
	return true;
}

static pg_attribute_always_inline bool
next_tid(SiftScanState *ss, ItemPointer tid)
{
	for (;;)
	{
		while (ss->mask == 0 && ss->wd < SIFT_OFFMASK_WORDS)
			ss->mask = ss->pm.offmask[ss->wd++];
		if (ss->mask)
		{
			ItemPointerSet(tid, ss->pm.blk, (ss->wd - 1) * 64 + pg_rightmost_one_pos64(ss->mask) + 1);
			ss->mask &= ss->mask - 1;
			return true;
		}
		if (!load_page(ss))
			return false;
	}
}

/* The phrase recheck of a heap-order candidate */
static bool
recheck_tuple(SiftScanState *ss)
{
	MemoryContext old = MemoryContextSwitchTo(ss->tmpcx);
	text	   *doc = index_text(ss);
	bool		ok = doc != NULL && sift_plan_recheck(ss->plan, doc);

	MemoryContextSwitchTo(old);
	MemoryContextReset(ss->tmpcx);
	return ok;
}

/* The tuple of the next heap-order match that passes the rechecks and the quals, and in *entry the index entry of the match */
static pg_attribute_always_inline TupleTableSlot *
next_row(SiftScanState *ss, ItemPointer entry)
{
	ItemPointerData tid;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	while (next_tid(ss, &tid))
	{
		if (!ss->need_heap)
			return ExecStoreVirtualTuple(ExecClearTuple(ss->css.ss.ss_ScanTupleSlot));
		*entry = tid;
		if (!sift_vis_fetch(&ss->vis, &tid, ss->heapslot) ||
			(ss->pm.recheck && !sift_page_sure(&ss->pm, ItemPointerGetOffsetNumber(&tid)) && !recheck_tuple(ss)))
			continue;
		econtext->ecxt_scantuple = fill_scan_slot(ss, 0);
		if (ss->quals && !ExecQual(ss->quals, econtext))
		{
			continue;
		}
		return econtext->ecxt_scantuple;
	}
	return NULL;
}

static TupleTableSlot *
unordered_next(SiftScanState *ss)
{
	ItemPointerData entry;

	return next_row(ss, &entry);
}

static TupleTableSlot *
ranked_next(SiftScanState *ss)
{
	ItemPointerData tid;
	float4		score;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	while (sift_rank_next(ss->rank, &tid, &score))
		if (sift_vis_fetch(&ss->vis, &tid, ss->heapslot))
		{
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = fill_scan_slot(ss, score);
			if (ExecQual(ss->cuts, econtext))
				return econtext->ecxt_scantuple;
		}
	return NULL;
}

/* ---------- keyed scan ---------- */

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
	if (nscored != nrows)
		elog(ERROR, "sift: the ranker scored " INT64_FORMAT " of the " INT64_FORMAT " matches the keys kept", nscored, nrows);
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

static void
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

static TupleTableSlot *
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

static TupleTableSlot *
count_next(SiftScanState *ss)
{
	TupleTableSlot *slot = ss->css.ss.ss_ScanTupleSlot;
	int64		n = ss->empty ? 0 :
		sift_eval_count_ranges(ss->plan, &ss->vis, scan_verify, ss, ss->par.sh ? sift_par_next_range : NULL, &ss->par);

	ss->done = true;
	ExecClearTuple(slot);
	for (int i = 0; i < ss->ncols; i++)
	{
		slot->tts_values[i] = Int64GetDatum(n);
		slot->tts_isnull[i] = false;
	}
	return ExecStoreVirtualTuple(slot);
}

static TupleTableSlot *
scan_next(ScanState *node)
{
	SiftScanState *ss = (SiftScanState *) node;

	if (ss->done)
		return NULL;
	if (!ss->started)
		start_scan(ss);
	if (ss->mode == SIFT_MODE_COUNT)
		return count_next(ss);
	if (ss->empty)
		return NULL;
	if (ss->mode == SIFT_MODE_RANKED)
		return ss->nkeys > 0 ? keyed_next(ss) : ranked_next(ss);
	return unordered_next(ss);
}

static bool
scan_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

/* ---------- executor methods ---------- */

/* The most rows a LIMIT $n [OFFSET $m] asks of a generic plan, evaluated now that the parameters are known */
static int64
limit_hint(SiftScanState *ss, List *limit)
{
	int64		k = 0;

	foreach_ptr(ExprState, e, limit)
	{
		bool		isnull;
		int64		n = DatumGetInt64(ExecEvalExprSwitchContext(e, ss->css.ss.ps.ps_ExprContext, &isnull));

		if (isnull || n < 0)
			return 0;
		k = Min(k + n, LIMIT_HINT_MAX);
	}
	return k;
}

/* The leading sort keys of a keyed scan: their expressions over the scan tuple, and how they compare */
static void
init_keys(SiftScanState *ss, CustomScan *cs)
{
	List	   *keys = lfourth_node(List, cs->custom_exprs),
			   *ops = list_nth_node(List, cs->custom_private, SIFT_PRIV_KEYOPS);
	int			i = 0;

	ss->nkeys = list_length(keys);
	ss->keyexpr = palloc(ss->nkeys * sizeof(ExprState *));
	ss->keysort = palloc0(ss->nkeys * sizeof(SortSupportData));
	ss->keylen = palloc(ss->nkeys * sizeof(int16));
	ss->keybyval = palloc(ss->nkeys * sizeof(bool));
	foreach_ptr(Expr, e, keys)
	{
		SortSupport ssup = &ss->keysort[i];

		ss->keyexpr[i] = ExecInitExpr(e, &ss->css.ss.ps);
		get_typlenbyval(exprType((Node *) e), &ss->keylen[i], &ss->keybyval[i]);
		ssup->ssup_cxt = CurrentMemoryContext;
		ssup->ssup_collation = DatumGetObjectId(list_nth_node(Const, ops, 3 * i + 1)->constvalue);
		ssup->ssup_nulls_first = DatumGetBool(list_nth_node(Const, ops, 3 * i + 2)->constvalue);
		PrepareSortSupportFromOrderingOp(DatumGetObjectId(list_nth_node(Const, ops, 3 * i)->constvalue), ssup);
		i++;
	}
}

static void
sift_begin(CustomScanState *node, EState *estate, int eflags)
{
	SiftScanState *ss = (SiftScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	ss->indexoid = DatumGetObjectId(((Const *) list_nth(cs->custom_private, SIFT_PRIV_INDEX_OID))->constvalue);
	ss->mode = DatumGetInt32(((Const *) list_nth(cs->custom_private, SIFT_PRIV_MODE))->constvalue);
	ss->k = DatumGetInt64(((Const *) list_nth(cs->custom_private, SIFT_PRIV_K))->constvalue);
	ss->heapoid = DatumGetObjectId(((Const *) list_nth(cs->custom_private, SIFT_PRIV_HEAP_OID))->constvalue);
	ss->gate = DatumGetFloat8(((Const *) list_nth(cs->custom_private, SIFT_PRIV_GATE))->constvalue);
	ss->gate_to = DatumGetFloat8(((Const *) list_nth(cs->custom_private, SIFT_PRIV_GATE_TO))->constvalue);
	ss->queries = ExecInitExprList(linitial_node(List, cs->custom_exprs), &node->ss.ps);
	ss->quals = ExecInitQual(lsecond_node(List, cs->custom_exprs), &node->ss.ps);
	ss->cuts = ExecInitQual(list_nth_node(List, cs->custom_exprs, 4), &node->ss.ps);
	init_keys(ss, cs);
	if (ss->k == 0 && !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ss->k = limit_hint(ss, ExecInitExprList(lthird_node(List, cs->custom_exprs), &node->ss.ps));
	ss->heap = node->ss.ss_currentRelation;
	if (ss->heap == NULL)
	{
		ss->heap = table_open(ss->heapoid, AccessShareLock);
		ss->heap_opened = true;
	}
	ss->index = index_open(ss->indexoid, AccessShareLock);
	ss->iinfo = BuildIndexInfo(ss->index);
	ss->heapslot = table_slot_create(ss->heap, &estate->es_tupleTable);
	ss->cxt = AllocSetContextCreate(estate->es_query_cxt, "sift scan", ALLOCSET_DEFAULT_SIZES);
	ss->tmpcx = AllocSetContextCreate(ss->cxt, "sift recheck", ALLOCSET_DEFAULT_SIZES);
	ss->wd = SIFT_OFFMASK_WORDS;
	ss->vm_sb = UINT32_MAX;
	init_cols(ss, cs);
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags));
	if (node->custom_ps)
		ss->bitmap = linitial(node->custom_ps);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		PredicateLockRelation(ss->heap, estate->es_snapshot);
}

static TupleTableSlot *
sift_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, scan_next, scan_recheck);
}

static void
sift_end(CustomScanState *node)
{
	SiftScanState *ss = (SiftScanState *) node;

	close_scan(ss);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	index_close(ss->index, NoLock);
	if (ss->heap_opened)
		table_close(ss->heap, NoLock);
	MemoryContextDelete(ss->cxt);
}

static void
sift_rescan(CustomScanState *node)
{
	SiftScanState *ss = (SiftScanState *) node;

	close_scan(ss);
	if (ss->bitmap)
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(ss->bitmap, node->ss.ps.chgParam);
		if (ss->bitmap->chgParam == NULL)
			ExecReScan(ss->bitmap);
	}
	ExecScanReScan(&node->ss);
}

/* The order a ranked scan hands up its rows in: its leading sort keys, if any, then the score */
static char *
order_text(SiftScanState *ss, CustomScan *cs, List *dctx)
{
	StringInfoData buf;
	int			i = 0;

	initStringInfo(&buf);
	foreach_ptr(Node, e, lfourth_node(List, cs->custom_exprs))
	{
		const SortSupportData *k = &ss->keysort[i++];

		appendStringInfoString(&buf, deparse_expression(e, dctx, false, false));
		if (k->ssup_reverse)
			appendStringInfoString(&buf, " DESC");
		if (k->ssup_nulls_first != k->ssup_reverse)
			appendStringInfoString(&buf, k->ssup_nulls_first ? " NULLS FIRST" : " NULLS LAST");
		appendStringInfoString(&buf, ", ");
	}
	appendStringInfoString(&buf, "score DESC");
	return buf.data;
}

/* The expressions as text, ANDed */
static char *
and_text(List *exprs, List *dctx)
{
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_ptr(Node, e, exprs)
		appendStringInfo(&buf, "%s%s", buf.len ? " AND " : "", deparse_expression(e, dctx, false, false));
	return buf.data;
}

static void
sift_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	SiftScanState *ss = (SiftScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *dctx = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan, ancestors);
	SiftTotals	t = ss->tot;

	ExplainPropertyText("Index", RelationGetRelationName(ss->index), es);
	ExplainPropertyText("Query", and_text(linitial_node(List, cs->custom_exprs), dctx), es);
	if (list_nth_node(List, cs->custom_exprs, 4) != NIL)
		ExplainPropertyText("Score Filter", and_text(list_nth_node(List, cs->custom_exprs, 4), dctx), es);
	if (ss->mode == SIFT_MODE_RANKED)
	{
		ExplainPropertyText("Order", order_text(ss, cs, dctx), es);
		if (ss->k > 0)
			ExplainPropertyInteger("Limit Hint", NULL, ss->k, es);
		if (ss->bitmap && gated(ss) && es->verbose)
		{
			ExplainPropertyFloat("Filter Mask From Matches", NULL, ss->gate, 0, es);
			if (!isinf(ss->gate_to))
				ExplainPropertyFloat("Filter Mask Up To Matches", NULL, ss->gate_to, 0, es);
		}
	}
	if (!es->analyze)
		return;
	if (ss->plan)
	{
		const SiftPlanStats *s = sift_plan_stats(ss->plan);

		t.sb_scanned += s->sb_scanned;
		t.blocks_scored += s->blocks_scored;
		t.blocks_skipped += s->blocks_skipped;
		t.candidates += s->candidates;
		t.verified += s->verified;
		t.heap_checks += ss->vis_open ? ss->vis.heap_checks : 0;
		t.heap_pages += ss->vis_open ? ss->vis.heap_pages : 0;
	}
	ExplainPropertyInteger("Index Searches", NULL, ss->nsearches, es);
	if (ss->nkeys > 0)
		ExplainPropertyInteger("Key Passes", NULL, ss->kpasses, es);
	if (ss->bitmap)
	{
		t.filter_sbs += ss->filter ? ss->filter->nsb : 0;
		t.filter_exact += ss->filter ? ss->filter->exact_pages : 0;
		t.filter_lossy += ss->filter ? ss->filter->lossy_pages : 0;
		ExplainPropertyInteger("Filter Superblocks", NULL, t.filter_sbs, es);
		ExplainPropertyInteger("Filter Pages Exact", NULL, t.filter_exact, es);
		ExplainPropertyInteger("Filter Pages Lossy", NULL, t.filter_lossy, es);
	}
	ExplainPropertyInteger("Superblocks", NULL, t.sb_scanned, es);
	if (ss->mode == SIFT_MODE_RANKED)
	{
		ExplainPropertyInteger("Blocks Scored", NULL, t.blocks_scored, es);
		ExplainPropertyInteger("Blocks Skipped", NULL, t.blocks_skipped, es);
	}
	if (ss->mode != SIFT_MODE_COUNT || t.candidates > 0)
		ExplainPropertyInteger("Candidates", NULL, t.candidates, es);
	ExplainPropertyInteger("Heap Checks", NULL, t.heap_checks, es);
	ExplainPropertyInteger("Heap Pages", NULL, t.heap_pages, es);
}

/* ---------- parallel ---------- */

/* Superblocks of the heap as of now: what a snapshot taken before sees lies in them */
static uint32
heap_superblocks(SiftScanState *ss)
{
	return ((uint64) sift_vis_nblocks(ss->heap) + SIFT_SB_PAGES - 1) >> SIFT_SB_SHIFT;
}

/*
 * Ranked: binds once for everyone. The participants score from this bind
 * (the leader too, when it runs the plan), so a writer cannot give them
 * different statistics or directories.
 */
static void
export_bind(SiftScanState *ss)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift parallel bind", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftIndexState *st = sift_state_open(ss->index, tmp);
	SiftPlan   *plan = bind_queries(ss, st, NULL);

	if (plan)
	{
		Size		len;
		void	   *data = sift_plan_export(plan, &len);

		sift_par_set_bind(ss->par.sh, ss->par.area, data, len);
	}
	sift_state_close(st);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}

static void
prepare_shared(SiftScanState *ss, int nworkers, bool rescan)
{
	if (rescan)
		sift_par_reinit(ss->par.sh, ss->par.area, nworkers, heap_superblocks(ss));
	else
		sift_par_init(ss->par.sh, nworkers, heap_superblocks(ss));
	if (ss->mode == SIFT_MODE_RANKED)
		export_bind(ss);
}

static Size
sift_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	return sift_par_estimate(pcxt->nworkers);
}

static void
sift_initialize_dsm(CustomScanState *node, ParallelContext *pcxt, void *coordinate)
{
	SiftScanState *ss = (SiftScanState *) node;

	ss->par = (SiftPar) {.sh = coordinate, .area = node->ss.ps.state->es_query_dsa};
	prepare_shared(ss, pcxt->nworkers, false);
}

static void
sift_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt, void *coordinate)
{
	SiftScanState *ss = (SiftScanState *) node;

	ss->par.area = node->ss.ps.state->es_query_dsa;
	prepare_shared(ss, pcxt->nworkers, true);
}

static void
sift_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	SiftScanState *ss = (SiftScanState *) node;

	ss->par = (SiftPar) {.sh = coordinate, .area = node->ss.ps.state->es_query_dsa};
}

static const CustomExecMethods sift_exec_methods = {
	.CustomName = "SiftScan",
	.BeginCustomScan = sift_begin,
	.ExecCustomScan = sift_exec,
	.EndCustomScan = sift_end,
	.ReScanCustomScan = sift_rescan,
	.ExplainCustomScan = sift_explain,
	.EstimateDSMCustomScan = sift_estimate_dsm,
	.InitializeDSMCustomScan = sift_initialize_dsm,
	.ReInitializeDSMCustomScan = sift_reinitialize_dsm,
	.InitializeWorkerCustomScan = sift_initialize_worker,
};

Node *
sift_create_scan_state(CustomScan *cscan)
{
	SiftScanState *ss = (SiftScanState *) newNode(sizeof(SiftScanState), T_CustomScanState);

	ss->css.methods = &sift_exec_methods;
	ss->css.flags = cscan->flags;
	return (Node *) ss;
}
