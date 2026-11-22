/*
 * agg/hybrid_agg_exec.c - the executor of SiftHybridAgg: the groups of an aggregate over one text query and structured quals, folded from the columns of the sift_col store (agg/v6/) with
 * no tuple of the scan and no heap fetch for a row the store decides, one tuple for each group when the first is asked for. B (agg/hybrid_agg_core.h): the hits of the text scan, in heap order
 * and unscored, make a bridge for each chunk of them and the store scan folds the rows of the bridge. A: the store scan fills the bridge of the candidates, the text scan reads the hits among
 * them, and the store scan folds the rows of the hits. The steps shared with SiftHybridScan are in hybrid_store.c.
 *
 * A row the stored values cannot decide (a value the leaf did not hold, a clause of the heap's: the residuals, a tenant EXISTS whose key set the store did not take) is decided by the heap's
 * version of the row before it is folded.
 *
 * check-memory: allow-allocs every array here follows one chunk of hits or one batch of rows, which the byte budget of the bridge and the granule bound
 */
#include "postgres.h"

#include "access/table.h"
#include "utils/date.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "catalog/index.h"
#include "commands/explain.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/hybrid_agg_core.h"
#include "agg/hybrid_state.h"
#include "agg/v6/v6.h"
#include "core/memtrack.h"
#include "core/compat.h"
#include "core/sift.h"

typedef struct HybridAggState
{
	HybridState h;
	SiftV6Spec *spec;
	TupleDesc	group_desc;
	int			ngroup;				/* the entries of the scan tuple the groups fill: the keys and the aggregates; the Vars of the quals that follow are NULL */
	MemoryContext gcx;
	SiftV6Group *group;
	SiftV6Fold *fold;
	SiftV6ScanStats fstats;
	bool		ran;
	bool		reads_score;		/* an aggregate over sift.score: the hits come ranked, in a bridge that keeps their scores (B) */
	bool		point_rows;			/* B with a few hits: the rows were read from the heap */
	int		   *point_leaf;
	bool		recheck_all;		/* a qual is the heap's: every row of the store is decided by it */
	SiftHybridRun *a_run;
	SiftHybridStats a_stats;
	SiftMatchIter *it;				/* B: the unscored hits of a text query the index decides alone */
	SiftPageMatch pm;
	int			pm_word;
	uint64		pm_bits;
} HybridAggState;

static void
fold_sink(void *arg, const SiftHybridBatch *b)
{
	HybridAggState *st = arg;
	const int	words = (b->n + 63) / 64;
	uint64	   *live = palloc(Max(words, 1) * sizeof(uint64)),
			   *maybe = palloc(Max(words, 1) * sizeof(uint64));

	for (int w = 0; w < words; w++)
	{
		maybe[w] = st->recheck_all ? b->live[w] : (b->maybe != NULL ? b->maybe[w] : 0);
		live[w] = b->live[w] & ~maybe[w];
	}
	sift_v6_fold_batch_scored(st->fold, b->tid, live, maybe, b->cols, b->score, b->n);
	pfree(live);
	pfree(maybe);
}

static bool
agg_recheck(void *arg, uint64 key)
{
	return sift_hybrid_heap_check(&((HybridAggState *) arg)->h, key, true);
}

static bool
iter_hit(void *arg, SiftKey *key, float4 *score)
{
	HybridAggState *st = arg;
	ItemPointerData tid;

	while (st->pm_bits == 0)
	{
		if (st->pm_word + 1 < SIFT_OFFMASK_WORDS)
			st->pm_bits = st->pm.offmask[++st->pm_word];
		else if (sift_iter_next(st->it, &st->pm))
			st->pm_bits = st->pm.offmask[st->pm_word = 0];
		else
			return false;
	}
	ItemPointerSet(&tid, st->pm.blk, st->pm_word * 64 + __builtin_ctzll(st->pm_bits) + 1);
	st->pm_bits &= st->pm_bits - 1;
	*key = sift_f2_tid(&tid);
	*score = 0;
	return true;
}

/* A: the text scan restricted to the bridge of the candidates */
static SiftHybridHitFn
candidate_hits(void *arg, const SiftTidBridge *cand, void **hits_arg)
{
	HybridState *h = &((HybridAggState *) arg)->h;

	sift_plan_set_tidbridge(h->plan, cand);
	h->rank = sift_rank_begin(h->plan, 0, sift_hybrid_verify_b, h);
	*hits_arg = h;
	return sift_hybrid_next_hit;
}

/* The fold vets the rows of a chunk (the visibility map, or the heap's version): the run need not vet them as well */
static bool
vetted_later(void *arg, SiftKey key, bool quals)
{
	return true;
}

static void
fold_b(HybridAggState *st)
{
	HybridState *h = &st->h;

	if (st->reads_score || sift_plan_recheck_needed(h->plan))
	{
		h->rank = sift_rank_begin(h->plan, 0, sift_hybrid_verify_b, h);
		sift_hybrid_agg_b(h->run, sift_hybrid_next_hit, h, st->reads_score, fold_sink, st);
		return;
	}
	st->it = sift_iter_begin(h->plan);
	st->pm_word = SIFT_OFFMASK_WORDS - 1;
	st->pm_bits = 0;
	sift_hybrid_agg_b(h->run, iter_hit, st, false, fold_sink, st);
	sift_iter_end(st->it);
	st->it = NULL;
}

/* B with a few hits: the heap's rows of the hits, as the columns of a batch, are folded; the store is not read (a granule decodes in more time than the heap gives the rows) */
#define POINT_BATCH		256

typedef struct PointCol
{
	int			attno;				/* of the heap */
	Oid			typid;
	int			scale;				/* a numeric: the digits after the point of the scaled integer the store holds */
	SiftV6Class cls;
	int64	   *i;
	double	   *f;
	const char **s;
	uint32	   *slen;
	bool	   *isnull;
} PointCol;

static bool
point_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID || typid == BOOLOID ||
		typid == FLOAT4OID || typid == FLOAT8OID || typid == TEXTOID || typid == VARCHAROID || typid == NUMERICOID;
}

static PointCol *
point_cols(HybridAggState *st, const SiftV6FoldCol *cols, int n)
{
	HybridState *h = &st->h;
	PointCol   *pc = palloc0(Max(n, 1) * sizeof(PointCol));

	for (int c = 0; c < n; c++)
	{
		pc[c].attno = h->store_index->rd_index->indkey.values[cols[c].attno - 1];
		pc[c].typid = TupleDescAttr(h->heapslot->tts_tupleDescriptor, pc[c].attno - 1)->atttypid;
		pc[c].cls = cols[c].cls;
		pc[c].scale = cols[c].scale;
		if (!point_type(pc[c].typid))
			return NULL;
		pc[c].i = palloc(POINT_BATCH * sizeof(int64));
		pc[c].f = palloc(POINT_BATCH * sizeof(double));
		pc[c].s = palloc(POINT_BATCH * sizeof(char *));
		pc[c].slen = palloc(POINT_BATCH * sizeof(uint32));
		pc[c].isnull = palloc(POINT_BATCH * sizeof(bool));
	}
	return pc;
}

/* The scaled integer the store holds of a numeric (the column's type is numeric(p, s) with p at most 18, so the value fits and has no digit past the scale) */
static int64
numeric_scaled(Datum d, int scale)
{
	int64		pow10 = 1;
	Datum		unit;

	for (int i = 0; i < scale; i++)
		pow10 *= 10;
	unit = DirectFunctionCall1(int8_numeric, Int64GetDatum(pow10));
	if (numeric_is_nan(DatumGetNumeric(d)) || numeric_is_inf(DatumGetNumeric(d)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a numeric value that is not a number cannot be folded from the store"),
						errhint("Run the statement with sift.enable_agg = off.")));
	return DatumGetInt64(DirectFunctionCall1(numeric_int8, DirectFunctionCall2(numeric_mul, d, unit)));
}

static void
point_put(HybridState *h, PointCol *c, int r)
{
	Datum		d = slot_getattr(h->heapslot, c->attno, &c->isnull[r]);

	if (c->isnull[r])
		return;
	switch (c->typid)
	{
		case INT2OID:
			c->i[r] = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			c->i[r] = DatumGetInt32(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			c->i[r] = DatumGetInt64(d);
			break;
		case NUMERICOID:
			c->i[r] = numeric_scaled(d, c->scale);
			break;
		case BOOLOID:
			c->i[r] = DatumGetBool(d);
			break;
		case FLOAT4OID:
			c->f[r] = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			c->f[r] = DatumGetFloat8(d);
			break;
		default:
			{
				text	   *t = DatumGetTextPP(d);

				c->slen[r] = VARSIZE_ANY_EXHDR(t);
				c->s[r] = memcpy(palloc(Max(c->slen[r], 1)), VARDATA_ANY(t), c->slen[r]);
			}
	}
}

static void
point_flush(HybridAggState *st, PointCol *pc, int ncols, const uint64 *keys, const float4 *scores, int n)
{
	uint64		live[POINT_BATCH / 64];
	ColView	   *views;
	int			nviews = 0;

	for (int c = 0; c < ncols; c++)
		nviews = Max(nviews, st->point_leaf[c] + 1);
	views = palloc0(nviews * sizeof(ColView));
	for (int c = 0; c < ncols; c++)
		views[st->point_leaf[c]] = (ColView) {.i = pc[c].i,.f = pc[c].f,.s = pc[c].s,.slen = pc[c].slen,.isnull = pc[c].isnull};
	memset(live, 0, sizeof(live));
	for (int r = 0; r < n; r++)
		live[r / 64] |= UINT64CONST(1) << (r % 64);
	sift_v6_fold_batch_scored(st->fold, keys, live, NULL, views, scores, n);
}

static void
fold_point(HybridAggState *st, const SiftV6FoldCol *cols, int ncols, PointCol *pc)
{
	HybridState *h = &st->h;
	uint64		keys[POINT_BATCH];
	float4		scores[POINT_BATCH];
	ItemPointerData hit,
				tid;
	float4		score;
	int			n = 0;

	st->point_leaf = palloc(Max(ncols, 1) * sizeof(int));
	for (int c = 0; c < ncols; c++)
		st->point_leaf[c] = cols[c].leaf;
	if (h->rank == NULL)
		h->rank = sift_rank_begin(h->plan, 0, sift_hybrid_verify_point, h);
	while (sift_rank_next(h->rank, &hit, &score))
	{
		tid = hit;
		if (!sift_vis_fetch(&h->vis, &tid, h->heapslot))
			continue;
		for (int c = 0; c < ncols; c++)
			point_put(h, &pc[c], n);
		scores[n] = score;
		keys[n++] = sift_f2_tid(&hit);
		if (n == POINT_BATCH)
		{
			point_flush(st, pc, ncols, keys, scores, n);
			n = 0;
		}
	}
	if (n > 0)
		point_flush(st, pc, ncols, keys, scores, n);
	st->point_rows = true;
}

/* A: the candidates of the store scan, the hits of the text scan among them, then the rows of the hits; a few candidates are read from the heap, as the standard plan does */
static void
fold_a(HybridAggState *st, PointCol *pc, const SiftV6FoldCol *cols, int ncols, Size budget)
{
	HybridState *h = &st->h;
	const SiftTidBridge *cand;

	st->a_run = sift_hybrid_begin(h->cxt, SIFT_HYB_COL_TO_BM25, &h->store, budget, sift_hybrid_heap_check, h);
	cand = sift_hybrid_a_bridge(st->a_run, false, false);
	if (!st->a_run->stats.empty && pc != NULL && st->a_run->stats.point)
	{
		sift_plan_set_tidbridge(h->plan, cand);
		h->rank = sift_rank_begin(h->plan, 0, sift_hybrid_verify_point, h);
		fold_point(st, cols, ncols, pc);
	}
	else if (!st->a_run->stats.empty)
		sift_hybrid_agg_a_from(h->run, cand, candidate_hits, st, fold_sink, st);
	st->a_stats = st->a_run->stats;
	sift_hybrid_end(st->a_run);
	st->a_run = NULL;
}

static void
fold_rows(HybridAggState *st, const SiftV6FoldCol *cols, int ncols)
{
	HybridState *h = &st->h;
	EState	   *estate = h->css.ss.ps.state;
	const Size	budget = (Size) work_mem * 1024 / 2;
	PointCol   *pc;

	if (st->reads_score)
	{
		h->dir = SIFT_HYB_BM25_TO_COL;
		h->resolved = h->adaptive;
	}
	else if (h->adaptive)
	{
		h->dir = sift_hybrid_pick_direction(h, true);
		h->resolved = true;
	}
	else if (h->dir == SIFT_HYB_COL_TO_BM25 && h->nscan == 0 && h->n_ext_preds == 0)
		h->dir = SIFT_HYB_BM25_TO_COL;
	st->recheck_all = !sift_hybrid_rows_exact(h);
	h->store.exact_quals = h->exact_quals;
	sift_vis_begin(&h->vis, h->heap, estate->es_snapshot, h->text_index, sift_plan_dir_version(h->plan));
	h->vis_open = true;
	h->run = sift_hybrid_begin(h->cxt, SIFT_HYB_BM25_TO_COL, &h->store, budget, vetted_later, h);
	pc = point_cols(st, cols, ncols);
	if (h->dir == SIFT_HYB_COL_TO_BM25)
		fold_a(st, pc, cols, ncols, budget);
	else if (pc != NULL && sift_plan_docs(h->plan) <= SIFT_TIDB_POINT_MAX)
		fold_point(st, cols, ncols, pc);
	else
		fold_b(st);
}

static void
agg_run(HybridAggState *st)
{
	HybridState *h = &st->h;
	EState	   *estate = h->css.ss.ps.state;
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	const SiftV6FoldCol *cols;
	int			ncols;

	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift hybrid agg groups");
	st->ran = true;
	MemoryContextSwitchTo(st->gcx);
	st->group = sift_v6_group_new(st->spec, st->gcx, st->group_desc);
	st->fold = sift_v6_fold_begin(st->spec, st->group, h->heap, h->store_index, estate->es_snapshot, &st->fstats, agg_recheck, st);
	st->reads_score = sift_v6_spec_reads_score(st->spec);
	cols = sift_v6_fold_cols(st->fold, &ncols);
	h->leaves = palloc(Max(ncols, 1) * sizeof(int));
	for (int c = 0; c < ncols; c++)
		h->leaves[c] = cols[c].leaf;
	h->nleaves = ncols;
	if (sift_hybrid_bind(h))
		fold_rows(st, cols, ncols);
	sift_hybrid_finish(h);
	sift_v6_fold_end(st->fold);
	sift_v6_group_finish(st->group);
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
agg_next(ScanState *ss)
{
	HybridAggState *st = (HybridAggState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->ran)
		agg_run(st);
	CHECK_FOR_INTERRUPTS();
	ExecClearTuple(slot);
	MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	if (!sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull))
		return NULL;
	for (int i = st->ngroup; i < slot->tts_tupleDescriptor->natts; i++)
		slot->tts_isnull[i] = true;
	return ExecStoreVirtualTuple(slot);
}

static bool
agg_recheck_slot(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
agg_exec(CustomScanState *node)
{
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot = ExecScan(&node->ss, agg_next, agg_recheck_slot);

	MemoryContextSwitchTo(old);
	return slot;
}

static void
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	HybridAggState *st = (HybridAggState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	TupleDesc	scan_desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	Oid			heap_oid = IndexGetRelation((Oid) list_nth_int(linitial(cs->custom_private), SIFT_HYBP_STORE_OID), false);

	st->spec = sift_v6_spec_unpack(lfourth(cs->custom_private));
	st->ngroup = intVal(list_nth(cs->custom_private, 4));
	st->group_desc = CreateTemplateTupleDesc(st->ngroup);
	for (int i = 0; i < st->ngroup; i++)
		TupleDescCopyEntry(st->group_desc, i + 1, scan_desc, i + 1);
	sift_tupdesc_finalize(st->group_desc);
	sift_hybrid_open(&st->h, cs, estate, table_open(heap_oid, AccessShareLock));
	st->h.heap_decide = false;
}

static void
free_groups(HybridAggState *st)
{
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
	st->group = NULL;
	st->gcx = NULL;
}

static void
agg_end(CustomScanState *node)
{
	HybridAggState *st = (HybridAggState *) node;
	Relation	heap = st->h.heap;

	sift_hybrid_finish(&st->h);
	free_groups(st);
	sift_hybrid_close(&st->h);
	table_close(heap, AccessShareLock);
}

static void
agg_rescan(CustomScanState *node)
{
	HybridAggState *st = (HybridAggState *) node;
	HybridState *h = &st->h;

	free_groups(st);
	MemoryContextReset(h->cxt);
	h->granules_seen = h->granules_skipped = h->parts_skipped = 0;
	h->empty = false;
	h->run = NULL;
	st->ran = false;
	st->point_rows = false;
	st->fstats = (SiftV6ScanStats) {0};
	st->a_stats = (SiftHybridStats) {0};
	ExecScanReScan(&node->ss);
}

static void
agg_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	HybridAggState *st = (HybridAggState *) node;
	const HybridState *h = &st->h;
	const SiftHybridStats *b = &h->stats;

	ExplainPropertyText("Hybrid Direction", h->adaptive ? (h->resolved ? psprintf("%s (adaptive)", sift_hybrid_dir_name(h->dir)) : "adaptive") : sift_hybrid_dir_name(h->dir), es);
	ExplainPropertyText("Hybrid Store", "sift_col", es);
	ExplainPropertyText("Index", RelationGetRelationName(h->store_index), es);
	if (!es->analyze)
		return;
	ExplainPropertyInteger("Bridge Chunks", NULL, b->chunks, es);
	ExplainPropertyInteger("Bridge TIDs", NULL, b->bridge_tids + st->a_stats.bridge_tids, es);
	ExplainPropertyInteger("Granules Skipped", NULL, h->granules_skipped, es);
	ExplainPropertyInteger("Granules Seen", NULL, h->granules_seen, es);
	ExplainPropertyInteger("Rows Folded", NULL, st->fstats.rows, es);
	ExplainPropertyInteger("Heap Vet Rows", NULL, st->fstats.heap_vet_rows, es);
	ExplainPropertyBool("Heap Decides Rows", st->recheck_all, es);
	ExplainPropertyBool("Heap Rows", st->point_rows, es);
	if (st->group != NULL)
		ExplainPropertyText("Group Mode", sift_v6_group_mode(st->group), es);
}

static const CustomExecMethods agg_exec_methods = {
	.CustomName = "SiftHybridAgg",
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

static Node *
agg_create_state(CustomScan *cscan)
{
	HybridAggState *st = (HybridAggState *) newNode(sizeof(HybridAggState), T_CustomScanState);

	st->h.css.methods = &agg_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_hybrid_agg_plan_methods = {"SiftHybridAgg", agg_create_state};
