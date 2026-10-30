/*
 * bm25/cscan.c - executor side of SiftScan (heap order or ranked) and
 * SiftCount.
 *
 * The scan tuple has the layout of custom_scan_tlist: needed heap columns,
 * then sift.score / sift.max_score values the scan computes; upper nodes and
 * the residual quals read it through INDEX_VAR. Visibility, residual quals and
 * the phrase text come from verify() callbacks into the heap, so the ranker
 * only verifies candidates that would enter the top-k.
 *
 * The scan itself: columns, the binds of the queries, the filter, the iteration and the rechecks;
 * cscan_exec.c has the executor hooks.
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
#include "core/explain.h"

#include "cscan_int.h"

/* The rows a keyed scan keeps in its first round when it has no hint */
#define KEYED_INITIAL_ROWS	1024
void
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
			SIFT_INVARIANT(false, "sift: unexpected scan tuple column");
		i++;
	}
}

/* The scan tuple of the heap tuple in heapslot; scores are those of the ranked hit */
TupleTableSlot *
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

/*
 * Visible, passes the residual quals, and with want_doc yields the indexed text. A late scan checks both on the rows it hands out
 * (ranked_next) and asks the ranker for more when they drop some, so a candidate that needs no text is not fetched here too.
 */
bool
scan_verify(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	SiftScanState *ss = arg;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	if (ss->late && !want_doc)
		return true;
	if (!sift_vis_fetch(&ss->vis, tid, ss->heapslot))
		return false;
	if (ss->quals && !ss->late)
	{
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = fill_scan_slot(ss, 0);
		if (!ExecQual(ss->quals, econtext))
			return false;
	}
	return !want_doc || (*doc = index_text(ss)) != NULL;
}

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
void
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

void
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

/* The query of an operand: its text, or the OR of an array's elements; NULL when the operand is NULL or an array holds no element (the operator is strict) */
static SiftQuery *
operand_query(SiftIndexState *st, Datum d, bool any)
{
	text	   *q;
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			n,
				m = 0;
	text	  **texts;

	if (!any)
	{
		q = DatumGetTextPP(d);
		return sift_state_query(st, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
	}
	arr = DatumGetArrayTypeP(d);
	deconstruct_array(arr, ARR_ELEMTYPE(arr), -1, false, TYPALIGN_INT, &elems, &nulls, &n);
	texts = palloc(Max(n, 1) * sizeof(text *));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			texts[m++] = DatumGetTextPP(elems[i]);
	return m > 0 ? sift_state_query_any(st, texts, m) : NULL;
}

/* The plan binds from the leader's export (shared), or reads the index itself; NULL: a query is NULL, no rows */
SiftPlan *
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

		if (isnull || (queries[i] = operand_query(st, d, ss->query_any[i])) == NULL)
			return NULL;
		i++;
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
bool
gated(const SiftScanState *ss)
{
	return ss->gate > 0 || !isinf(ss->gate_to);
}

static bool
in_gate(const SiftScanState *ss, double docs)
{
	return docs >= ss->gate && docs <= ss->gate_to;
}

static void
start_scan(SiftScanState *ss)
{
	EState	   *estate = ss->css.ss.ps.state;
	MemoryContext old;

	ss->started = true;
	ss->nsearches++;
	ss->late = ss->mode == SIFT_MODE_RANKED && ss->nkeys == 0 && ss->quals != NULL && ss->late_rows > 0 && !ss->secure && !ss->par.sh;
	ss->nread = ss->nkept = 0;
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
pg_attribute_always_inline TupleTableSlot *
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

	for (;;)
	{
		if (ss->late)
			sift_rank_feedback(ss->rank, ss->nread, ss->nkept, ss->late_rows - ss->nkept);
		if (!sift_rank_next(ss->rank, &tid, &score))
			return NULL;
		ss->nread++;
		if (!sift_vis_fetch(&ss->vis, &tid, ss->heapslot))
			continue;
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = fill_scan_slot(ss, score);
		if (ExecQual(ss->cuts, econtext) && (!ss->late || ExecQual(ss->quals, econtext)))
		{
			ss->nkept++;
			return econtext->ecxt_scantuple;
		}
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

TupleTableSlot *
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

bool
scan_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

