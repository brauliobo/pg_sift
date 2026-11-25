/*
 * bm25/cscan/cscan.c - executor side of SiftScan (heap order or ranked) and
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

#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/rel.h"

#include "bm25/eval/eval.h"
#include "bm25/doc.h"
#include "bm25/planner/planner.h"
#include "bm25/cscan/int.h"
#include "core/quarantine/quarantine.h"

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
	Datum		values[INDEX_MAX_KEYS];	/* the key and the INCLUDE columns */
	bool		isnull[INDEX_MAX_KEYS];

	ResetPerTupleExprContext(estate);
	GetPerTupleExprContext(estate)->ecxt_scantuple = ss->heapslot;
	FormIndexDatum(ss->iinfo, ss->heapslot, estate, values, isnull);
	return sift_bm25_assemble_doc(ss->index, values, isnull, CurrentMemoryContext);
}

/*
 * Is the candidate's page all-visible, so that the candidate is visible without a fetch? Only a scan whose hits go to an incremental sort under a LIMIT (ties) asks: the sort
 * orders the rows tied with the last by their own keys, so the ctid of the tuple a HOT chain ends in, which a fetch would tell, orders nothing, and ranked_next fetches the
 * rows it hands out.
 */
static bool
all_visible_candidate(SiftScanState *ss, ItemPointer tid)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	uint32		sb = sift_block_sb(blk),
				page = blk & (SIFT_SB_PAGES - 1);

	if (!ss->ties || ss->quals || !ss->vis.use_vm || ss->heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		return false;
	if (sb != ss->vm_sb)
	{
		ss->vm_sb = sb;
		sift_vis_sb_mask(&ss->vis, sb, ss->allvis);
	}
	return ss->allvis[page / 64] >> (page % 64) & 1;
}

/*
 * Visible, passes the residual quals, and with want_doc yields the indexed text. A late scan checks both on the rows it hands out
 * (ranked_next) and asks the ranker for more when they drop some, so a candidate that needs no text is not fetched here too.
 */
/* The heap check of a candidate or a hit of the ranker; a tuple dead to every snapshot is one the ranker keeps out of its next searches */
bool
ranked_tuple_visible(SiftScanState *ss, ItemPointer tid)
{
	bool		call_again = false,
				all_dead = false;

	ss->vis.heap_checks++;
	if (table_index_fetch_tuple(ss->vis.fetch, tid, ss->vis.snapshot, ss->heapslot, &call_again, &all_dead))
		return true;
	if (all_dead && ss->rank)
		sift_rank_dead_tid(ss->rank, tid);
	return false;
}

bool
scan_verify(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	SiftScanState *ss = arg;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	if (ss->late && !want_doc)
		return true;
	if (!want_doc && all_visible_candidate(ss, tid))
		return true;
	if (!ranked_tuple_visible(ss, tid))
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
	ss->tot.searches += s->searches;
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
	/* what a rescan must keep is not a child of what it resets (a recheck after it works in a context that is gone) */
	SIFT_INVARIANT(MemoryContextGetParent(ss->tmpcx) == ss->css.ss.ps.state->es_query_cxt, "the recheck context of a scan does not outlive its rescan");
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

/* Parallel scans read the directory the leader exported, and ranked ones score from the bind it made, whoever runs them */
static void
bind_scan(SiftScanState *ss)
{
	Size		len;
	const void *shared = ss->par.sh ? sift_par_get_bind(&ss->par, &len) : NULL;

	ss->st = sift_state_open_dir(ss->index, ss->cxt, shared ? sift_shared_dir(shared) : NULL);
	sift_quarantine_net(ss->index, ss->st->dir->meta.flags);
	ss->plan = bind_queries(ss, ss->st, ss->mode == SIFT_MODE_RANKED ? shared : NULL);
	ss->empty = ss->plan == NULL;
}

/*
 * The bitmap of the tuples the other quals keep becomes the masks the ranker restricts itself to. Its
 * node runs in the executor's context like any child; no tuple in it, no rows.
 */
static SiftFilter *
run_bitmap(SiftScanState *ss)
{
	EState	   *estate = ss->css.ss.ps.state;
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	TIDBitmap  *tbm = (TIDBitmap *) MultiExecProcNode(ss->bitmap);
	SiftFilter *filter;

	MemoryContextSwitchTo(old);
	filter = sift_filter_build(tbm, (Size) work_mem * 1024, ss->cxt);
	tbm_free(tbm);
	return filter;
}

/* The participants of a parallel scan share one filter: the first builds and publishes it, the others read it */
static void
build_filter(SiftScanState *ss)
{
	if (!ss->par.sh)
		ss->filter = run_bitmap(ss);
	else if (sift_par_filter_claim(&ss->par))
	{
		ss->filter = run_bitmap(ss);
		sift_par_filter_publish(&ss->par, ss->filter);
	}
	else
		ss->filter = sift_par_filter_attach(&ss->par, ss->cxt);
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
	pgstat_count_index_scan(ss->index);
	ss->late = ss->mode == SIFT_MODE_RANKED && ss->nkeys == 0 && ss->quals != NULL && ss->late_rows > 0 && !ss->secure && !ss->par.sh;
	ss->nread = ss->nkept = 0;
	if (ss->bitmap && !gated(ss))
		build_filter(ss);
	old = MemoryContextSwitchTo(ss->cxt);
	if (!ss->empty)
		bind_scan(ss);
	load_semi(ss);
	if (ss->plan && ss->nattr)
		sift_plan_set_attrs(ss->plan, ss->attr, ss->nattr);
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
			if (ss->ties)
				sift_rank_ties(ss->rank);
			if (ss->group_floor)
				sift_rank_group_floor(ss->rank, ss->group_floor);
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
		if (ranked_tuple_visible(ss, &tid))
		{
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = fill_scan_slot(ss, score);
			if (ExecQual(ss->cuts, econtext) && (!ss->late || ExecQual(ss->quals, econtext)))
			{
				ss->nkept++;
				return econtext->ecxt_scantuple;
			}
		}
		/* a row less than the hits: the k rows the sort wants are not the k first hits' ties any more */
		sift_rank_reopen(ss->rank);
	}
}

static TupleTableSlot *
count_next(SiftScanState *ss)
{
	TupleTableSlot *slot = ss->css.ss.ss_ScanTupleSlot;
	int64		n = ss->empty ? 0 :
		sift_eval_count_ranges(ss->plan, &ss->vis, scan_verify, ss, ss->par.sh ? sift_par_next_range : NULL, &ss->par);

	ss->done = true;
	pgstat_count_index_tuples(ss->index, n);
	ExecClearTuple(slot);
	for (int i = 0; i < ss->ncols; i++)
	{
		slot->tts_values[i] = Int64GetDatum(n);
		slot->tts_isnull[i] = false;
	}
	return ExecStoreVirtualTuple(slot);
}

static TupleTableSlot *
next_slot(SiftScanState *ss)
{
	if (ss->empty)
		return NULL;
	if (ss->mode == SIFT_MODE_RANKED)
		return ss->nkeys > 0 ? keyed_next(ss) : ranked_next(ss);
	return unordered_next(ss);
}

TupleTableSlot *
scan_next(ScanState *node)
{
	SiftScanState *ss = (SiftScanState *) node;
	TupleTableSlot *slot;

	if (ss->done)
		return NULL;
	if (!ss->started)
		start_scan(ss);
	if (ss->mode == SIFT_MODE_COUNT)
		return count_next(ss);
	slot = next_slot(ss);
	if (slot != NULL)
		pgstat_count_index_tuples(ss->index, 1);
	return slot;
}

bool
scan_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

/* Does an EvalPlanQual recheck hand this scan a row version of its relation (or has it, already) instead of letting it read? */
bool
epq_substituted(const SiftScanState *ss)
{
	const EPQState *epq = ss->css.ss.ps.state->es_epq_active;
	Index		rti = ((const Scan *) ss->css.ss.ps.plan)->scanrelid;

	return epq != NULL && rti > 0 &&
		(epq->relsubs_done[rti - 1] || epq->relsubs_slot[rti - 1] != NULL || epq->relsubs_rowmark[rti - 1] != NULL);
}

/* The score of the row version in heapslot, from its text; false when it no longer matches the queries (or has none) */
static bool
epq_score(SiftScanState *ss, float4 *score)
{
	MemoryContext old = MemoryContextSwitchTo(ss->tmpcx);
	text	   *doc;
	bool		ok;

	if (!ss->started)
	{
		bool		max = false;

		for (int i = 0; i < ss->ncols; i++)
			max |= ss->col[i].kind == COL_MAX_SCORE;
		MemoryContextSwitchTo(old);
		if (max)
			start_scan(ss);
		else
		{
			ss->started = true;
			old = MemoryContextSwitchTo(ss->cxt);
			bind_scan(ss);
			MemoryContextSwitchTo(old);
		}
		old = MemoryContextSwitchTo(ss->tmpcx);
	}
	doc = ss->empty ? NULL : index_text(ss);
	ok = doc != NULL && sift_plan_eval_doc(ss->plan, doc, score);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ss->tmpcx);
	return ok;
}

/*
 * EvalPlanQual: the row version a concurrent update left, which the caller locked, replaces the rows the scan reads. It is scored from
 * its own text, so the match, the score and the quals are those of that version (a text that no longer matches is no row). The test
 * tuple is of the relation's layout, not the scan tuple's, so ExecScan cannot hand it up: this builds the scan tuple and projects it.
 */
TupleTableSlot *
epq_next(SiftScanState *ss)
{
	EPQState   *epq = ss->css.ss.ps.state->es_epq_active;
	Index		rti = ((Scan *) ss->css.ss.ps.plan)->scanrelid;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;
	TupleTableSlot *test = epq->relsubs_slot[rti - 1];
	float4		score = 0;

	if (epq->relsubs_done[rti - 1])
		return NULL;
	epq->relsubs_done[rti - 1] = true;
	if (test != NULL ? TupIsNull(test) : !EvalPlanQualFetchRowMark(epq, rti, ss->heapslot) || TupIsNull(ss->heapslot))
		return NULL;
	if (test != NULL)
		ExecCopySlot(ss->heapslot, test);
	if (!(ss->epq_score ? ss->epq_score : epq_score) (ss, &score))
		return NULL;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = fill_scan_slot(ss, score);
	if (!ExecQual(ss->cuts, econtext) || !ExecQual(ss->quals, econtext))
		return NULL;
	return ss->css.ss.ps.ps_ProjInfo ? ExecProject(ss->css.ss.ps.ps_ProjInfo) : econtext->ecxt_scantuple;
}

