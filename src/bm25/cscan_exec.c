/*
 * bm25/cscan_exec.c - executor side of SiftScan and SiftCount: the LIMIT hint, begin, exec, end, rescan, EXPLAIN and the parallel (DSM) hooks.
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
	ss->late_rows = DatumGetInt32(((Const *) list_nth(cs->custom_private, SIFT_PRIV_LATE))->constvalue);
	ss->heapoid = DatumGetObjectId(((Const *) list_nth(cs->custom_private, SIFT_PRIV_HEAP_OID))->constvalue);
	ss->gate = DatumGetFloat8(((Const *) list_nth(cs->custom_private, SIFT_PRIV_GATE))->constvalue);
	ss->gate_to = DatumGetFloat8(((Const *) list_nth(cs->custom_private, SIFT_PRIV_GATE_TO))->constvalue);
	ss->queries = ExecInitExprList(linitial_node(List, cs->custom_exprs), &node->ss.ps);
	ss->query_any = palloc(Max(list_length(ss->queries), 1) * sizeof(bool));
	foreach_ptr(Node, q, linitial_node(List, cs->custom_exprs))
		ss->query_any[foreach_current_index(q)] = OidIsValid(get_element_type(exprType(q)));
	ss->quals = ExecInitQual(lsecond_node(List, cs->custom_exprs), &node->ss.ps);
	ss->cuts = ExecInitQual(list_nth_node(List, cs->custom_exprs, 4), &node->ss.ps);
	ss->secure = list_length(cs->custom_exprs) > SIFT_EXPR_SECURE;
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
	SiftExplainLine ln;

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
	/* the counters of the index count the rows the policy hides too: the rows of the plan node are what the role may see */
	if (!es->analyze || ss->secure)
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
	if (ss->bitmap)
	{
		t.filter_sbs += ss->filter ? ss->filter->nsb : 0;
		t.filter_exact += ss->filter ? ss->filter->exact_pages : 0;
		t.filter_lossy += ss->filter ? ss->filter->lossy_pages : 0;
	}
	sift_explain_begin(&ln, es, "Scan");
	sift_explain_int(&ln, "Index Searches", NULL, "index searches", ss->nsearches, SIFT_XI_KEEP);
	if (ss->nkeys > 0)
		sift_explain_int(&ln, "Key Passes", NULL, "key passes", ss->kpasses, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Superblocks", NULL, "superblocks", t.sb_scanned, SIFT_XI_KEEP);
	if (ss->mode == SIFT_MODE_RANKED)
	{
		sift_explain_int(&ln, "Blocks Scored", NULL, "blocks scored", t.blocks_scored, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Blocks Skipped", NULL, "skipped", t.blocks_skipped, SIFT_XI_SUB | SIFT_XI_KEEP);
	}
	if (ss->mode != SIFT_MODE_COUNT || t.candidates > 0)
		sift_explain_int(&ln, "Candidates", NULL, "candidates", t.candidates, SIFT_XI_KEEP);
	sift_explain_int(&ln, "Heap Checks", NULL, "heap checks", t.heap_checks, 0);
	sift_explain_int(&ln, "Heap Pages", NULL, "heap pages", t.heap_pages, 0);
	sift_explain_end(&ln);
	if (ss->bitmap)
	{
		sift_explain_begin(&ln, es, "Filter Mask");
		sift_explain_int(&ln, "Filter Superblocks", NULL, "superblocks", t.filter_sbs, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Filter Pages Exact", NULL, "exact pages", t.filter_exact, SIFT_XI_KEEP);
		sift_explain_int(&ln, "Filter Pages Lossy", NULL, "lossy pages", t.filter_lossy, SIFT_XI_KEEP);
		sift_explain_end(&ln);
	}
}

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
