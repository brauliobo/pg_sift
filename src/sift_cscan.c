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
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/tidbitmap.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

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

typedef struct SiftScanState
{
	CustomScanState css;
	SiftScanMode mode;
	Oid			indexoid;
	Oid			heapoid;
	int64		k;
	List	   *queries;			/* ExprState */
	ExprState  *quals;				/* residual quals over the scan tuple */
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

static void
close_scan(SiftScanState *ss)
{
	add_stats(ss);
	if (ss->rank)
		sift_rank_end(ss->rank);
	if (ss->it)
		sift_iter_end(ss->it);
	if (ss->vis_open)
		sift_vis_end(&ss->vis);
	if (ss->st)
		sift_state_close(ss->st);
	ss->rank = NULL;
	ss->it = NULL;
	ss->st = NULL;
	ss->plan = NULL;
	ss->filter = NULL;
	ss->vis_open = false;
	MemoryContextReset(ss->cxt);
	ss->started = ss->done = ss->empty = false;
	ss->ahead_pos = ss->ahead_n = 0;
	ss->ahead_blk = 0;
	ss->ahead_done = false;
	ss->wd = SIFT_OFFMASK_WORDS;
	ss->mask = 0;
	ss->vm_sb = UINT32_MAX;
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
		if (ss->mode == SIFT_MODE_RANKED)
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

static bool
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

static TupleTableSlot *
unordered_next(SiftScanState *ss)
{
	ItemPointerData tid;
	ExprContext *econtext = ss->css.ss.ps.ps_ExprContext;

	while (next_tid(ss, &tid))
	{
		if (!ss->need_heap)
			return ExecStoreVirtualTuple(ExecClearTuple(ss->css.ss.ss_ScanTupleSlot));
		if (!sift_vis_fetch(&ss->vis, &tid, ss->heapslot) ||
			(ss->pm.recheck && !sift_page_sure(&ss->pm, ItemPointerGetOffsetNumber(&tid)) && !recheck_tuple(ss)))
			continue;
		econtext->ecxt_scantuple = fill_scan_slot(ss, 0);
		if (ss->quals && !ExecQual(ss->quals, econtext))
		{
			InstrCountFiltered1(ss, 1);
			continue;
		}
		return econtext->ecxt_scantuple;
	}
	return NULL;
}

static TupleTableSlot *
ranked_next(SiftScanState *ss)
{
	ItemPointerData tid;
	float4		score;

	while (sift_rank_next(ss->rank, &tid, &score))
		if (sift_vis_fetch(&ss->vis, &tid, ss->heapslot))
			return fill_scan_slot(ss, score);
	return NULL;
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
	return ss->mode == SIFT_MODE_RANKED ? ranked_next(ss) : unordered_next(ss);
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

static void
sift_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	SiftScanState *ss = (SiftScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *dctx = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan, ancestors);
	StringInfoData qs;
	SiftTotals	t = ss->tot;

	initStringInfo(&qs);
	foreach_ptr(Node, q, linitial_node(List, cs->custom_exprs))
		appendStringInfo(&qs, "%s%s", qs.len ? " AND " : "", deparse_expression(q, dctx, false, false));
	ExplainPropertyText("Index", RelationGetRelationName(ss->index), es);
	ExplainPropertyText("Query", qs.data, es);
	if (ss->mode == SIFT_MODE_RANKED)
	{
		ExplainPropertyText("Order", "score DESC", es);
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
