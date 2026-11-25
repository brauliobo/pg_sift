/*
 * col/f2/store/ops_hybrid.c - SiftHybridOps of format major 2: the scan of the store a BM25 text scan joins (SiftHybridScan names what it reads), the survey of the granules the structured quals
 * leave, and the fold of the rows of a scan into the groups of an aggregate (f2_v6_scan.c). A sift_table's store decides visibility and has no undecided rows or scores; an index's scan hands the
 * rows over with the ones a leaf could not decide and the scores of the bridge.
 */
#include "postgres.h"

#include "access/genam.h"

#include "agg/hybrid/hybrid.h"
#include "col/f2/sink/sink.h"
#include "col/f2/store/store.h"
#include "col/f2/store/ops_int.h"
#include "col/f2/store/scan.h"
#include "col/f2/v6/scan.h"

typedef struct HybridSinkArg
{
	void		(*sink) (void *arg, const SiftHybridBatch *batch);
	void	   *arg;
	bool		table;
} HybridSinkArg;

static void
hybrid_sink(void *arg, const SiftF2Batch *b)
{
	HybridSinkArg *a = arg;
	const int	words = (b->n + 63) / 64;
	uint64	   *any;
	SiftHybridBatch hb;

	if (a->table)
	{
		hb = (SiftHybridBatch) {.tid = b->tid,.live = b->live,.n = b->n,.exact_vis = true,.cols = b->views};
		a->sink(a->arg, &hb);
		return;
	}
	any = palloc(Max(words, 1) * sizeof(uint64));
	hb = (SiftHybridBatch) {.tid = b->tid,.live = any,.maybe = b->maybe,.score = b->scores,.n = b->n,.exact_vis = false,.cols = b->views};
	for (int w = 0; w < words; w++)
		any[w] = b->live[w] | b->maybe[w];
	a->sink(a->arg, &hb);
	pfree(any);
}

/* the columns the sink reads are folded by the fold of the aggregate, which takes the NaN of a numeric or refuses it for the aggregate that cannot (f2_v6_scan_fill.c) */
static bool *
folded_leaves(const SiftHybridScan *s, const SiftF2Store *store)
{
	bool	   *nan_leaves = palloc0(Max(store->ncols, 1) * sizeof(bool));

	for (int i = 0; i < s->nleaves; i++)
		nan_leaves[s->leaves[i]] = true;
	return nan_leaves;
}

static void
table_scan(const SiftHybridScan *s, HybridSinkArg *a)
{
	const SiftStoreQuals *q = (const SiftStoreQuals *) s->quals;
	SiftF2Store *store = sift_f2_store_open(s->heap);
	SiftStoreScanOpts o = {.rel = s->heap,.store = store,.snap = s->snap,.preds = q->preds,.npreds = q->npreds,.leaves = s->leaves,.nleaves = s->nleaves,
		.nan_leaves = folded_leaves(s, store),.bridge = s->bridge,.bridge_seen = s->granules_seen,.bridge_skipped = s->granules_skipped};

	if (q->possible)
		sift_f2_store_scan_sink(&o, hybrid_sink, a);
}

static IndexScanDesc
index_scan(const SiftHybridScan *s)
{
	IndexScanDesc scan = sift_index_beginscan(s->heap, s->index, s->snap, s->nkeys);

	index_rescan(scan, s->keys, s->nkeys, NULL, 0);
	return scan;
}

static void
hybrid_scan(const SiftHybridScan *s, void (*sink) (void *arg, const SiftHybridBatch *batch), void *arg)
{
	HybridSinkArg a = {sink, arg, s->index == NULL};
	IndexScanDesc scan;

	if (a.table)
	{
		table_scan(s, &a);
		return;
	}
	scan = index_scan(s);
	if (s->n_ext_preds > 0 || s->bridge)
		sift_f2_scan_set_ext_preds(scan, s->ext_preds, s->n_ext_preds, s->parts_skipped);
	if (s->bridge)
	{
		sift_f2_scan_set_bridge(scan, s->bridge, s->granules_seen, s->granules_skipped);
		if (s->heap_decide)
			sift_f2_scan_set_heap_decide(scan);
	}
	sift_f2_scan_sink_maybe(scan, hybrid_sink, &a, s->leaves, s->nleaves);
	index_endscan(scan);
}

static uint64
hybrid_survey(const SiftHybridScan *s)
{
	IndexScanDesc scan = index_scan(s);
	uint64		n;

	if (s->n_ext_preds > 0)
		sift_f2_scan_set_ext_preds(scan, s->ext_preds, s->n_ext_preds, NULL);
	n = sift_f2_scan_survey(scan);
	index_endscan(scan);
	return n;
}

static bool
hybrid_table_has_preds(const struct SiftStoreQuals *q)
{
	return ((const SiftStoreQuals *) q)->npreds > 0;
}

const SiftHybridOps sift_f2_hybrid_ops = {
	.scan = hybrid_scan,
	.survey = hybrid_survey,
	.table_has_preds = hybrid_table_has_preds,
	.fold_begin = sift_v6_fold_begin,
	.fold_cols = sift_v6_fold_cols,
	.fold_batch_scored = sift_v6_fold_batch_scored,
	.fold_end = sift_v6_fold_end
};
