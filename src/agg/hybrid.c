/*
 * agg/hybrid.c - the cost choice and the two drivers of SiftHybridScan (agg/hybrid.h): what is portable of it, the executor and the planner of PostgreSQL are
 * hybrid_exec.c and hybrid_plan.c.
 *
 * check-memory: allow-allocs every array here follows one chunk of hits or one batch of rows, which the byte budget of the bridge and the granule bound
 */
#include "core/host.h"

#include <float.h>
#include <math.h>

#include "agg/hybrid.h"
#include "core/memtrack.h"

/* The cost of one row or granule of each step, in the units of PostgreSQL's cpu_tuple_cost = 0.01 scaled by 0.05 (the vectors of a compressed granule) */
#define HYB_F2_ROW			0.00025		/* a predicate on the compressed chunks of a row of a surviving granule */
#define HYB_BRIDGE_ADD		0.0005		/* one TID into the bridge */
#define HYB_BM25_DOC		0.002		/* one matched document, scored */
#define HYB_BM25_UNSCORED	0.35		/* the fraction of it that an unordered match costs: no term frequency is decoded, nothing scored, no queue */
#define HYB_POINT_TID		0.01		/* a TID read directly (bridge of at most SIFT_TIDB_POINT_MAX) */
#define HYB_TIX_GRANULE		0.00002		/* the TID index test of a granule */
#define HYB_TID_DECODE		0.00005		/* the _tid of a row of a surviving granule */
#define HYB_B_HIT			0.008		/* a hit of the bridge of direction B, from the page record to the fold (measured: 0.45 us a hit, with the match scored) */
#define HYB_B_PASSES		2			/* passes over the store of a ranked scan that stops after a LIMIT: the first chunk of hits seldom holds the rows */
#define HYB_SORT_CMP		0.0025		/* one comparison of the sort of the rows of a chunk */
#define HYB_PAYLOAD_ROW		0.0003		/* the other columns of a row the bridge kept */
#define HYB_SETUP			4.0			/* the least a hybrid scan costs: it opens the text index, the store and the heap visibility before the first row, 0.15 ms measured, the whole of a standard plan that reaches a few dozen rows */
#define HYB_HIT_BYTES		64			/* what the ranked search holds for one hit that no LIMIT bounds */

/* Bytes of the byte budget one hit of a chunk takes at the most: a page record, an ARRAY slot, its pending score, the map of the pages at its fullest */
#define HYB_B_BYTES_PER_HIT	192
#define HYB_B_FIRST_HITS(k)	Max((uint64) 4 * (k), 256)	/* hits of the first chunk of a ranked scan that stops after k rows */
#define HYB_MIN_BUDGET		((Size) 64 * 1024)

double
sift_hybrid_granule_hit(double sel, double rows_per_granule)
{
	return 1.0 - pow(1.0 - Min(Max(sel, 0.0), 1.0), Max(rows_per_granule, 1.0));
}

static double
bm25_doc(const SiftHybridEst *e)
{
	return e->agg_mode && !e->reads_score ? HYB_BM25_DOC * HYB_BM25_UNSCORED : HYB_BM25_DOC;
}

static double
cost_col_to_bm25(const SiftHybridEst *e)
{
	double		rpg = e->rows / Max(e->granules, 1.0),
				kept = e->rows * e->sel_col,
				matched = kept * e->sel_bm25,
				wanted = e->limit > 0 ? Min(matched, e->limit) : matched,
				scan = e->col_granules * rpg * HYB_F2_ROW,
				rank = kept <= SIFT_TIDB_POINT_MAX ? kept * HYB_POINT_TID : matched * bm25_doc(e),
				verify = e->host == SIFT_HYB_HOST_TABLE || e->agg_mode ? 0.0 : wanted * e->heap_fetch;

	return e->bm25_dict_cost + scan + kept * HYB_BRIDGE_ADD + rank + verify;
}

/* The rows of the bridge that the store keeps; only these are fetched when the heap has a column or a qual the store does not */
static double
cost_bm25_to_col(const SiftHybridEst *e)
{
	double		rpg = e->rows / Max(e->granules, 1.0),
				matched = e->rows * e->sel_bm25,
				hits = e->ranked && e->limit > 0 ? Min(matched, 2.0 * e->limit / Max(e->sel_col, 1e-6)) : matched,
				surviving = e->granules * sift_hybrid_granule_hit(hits / Max(e->rows, 1.0), rpg),
				kept = hits * e->sel_col,
				fetch = !e->covered || e->has_residual ? (e->limit > 0 ? Min(kept, e->limit) : kept) * e->heap_fetch : 0.0,
				vis = fetch > 0 || e->host == SIFT_HYB_HOST_TABLE ? 0.0 : (1.0 - e->allvisfrac) * kept * e->heap_fetch,
				passes = e->ranked && e->limit > 0 ? HYB_B_PASSES : 1,
				sort = e->ranked && !e->agg_mode ? kept * log2(Max(kept, 2.0)) * HYB_SORT_CMP : 0.0;

	if (matched <= SIFT_TIDB_POINT_MAX)
		return e->bm25_dict_cost + matched * (bm25_doc(e) + HYB_POINT_TID) + fetch;
	return e->bm25_dict_cost + hits * (bm25_doc(e) + HYB_BRIDGE_ADD + HYB_B_HIT) + passes * (e->granules * HYB_TIX_GRANULE + surviving * rpg * HYB_TID_DECODE) + kept * HYB_PAYLOAD_ROW +
		fetch + vis + sort;
}

/* The standard plan reads the rows of the match (the text index first), or those of the key set the tenant's EXISTS gives, whichever are fewer where an index reaches them */
static double
cost_standard(const SiftHybridEst *e)
{
	double		matched = e->rows * e->sel_bm25,
				fetched = e->ranked && e->limit > 0 ? Min(matched, e->limit / Max(e->sel_col, 1e-6)) : matched,
				text_first = fetched * (HYB_BM25_DOC + e->heap_fetch);

	return e->bm25_dict_cost + (e->probeable ? Min(text_first, (e->probe_rows + 1.0) * (HYB_BM25_DOC + e->heap_fetch)) : text_first);
}

bool
sift_hybrid_beats(double cost, const SiftHybridCosts *c)
{
	return Max(cost, HYB_SETUP) < c->standard * SIFT_HYB_WIN;
}

SiftHybridDir
sift_hybrid_choose(const SiftHybridEst *e, SiftHybridCosts *costs)
{
	SiftHybridCosts c = {DBL_MAX, DBL_MAX, cost_standard(e)};
	SiftHybridDir dir = SIFT_HYB_NONE;
	bool		bounded = e->limit > 0 || e->hit_budget <= 0,
				quals = e->has_col_quals || e->sel_acl < 1.0;

	if (quals && (bounded || e->rows * e->sel_col * e->sel_bm25 * HYB_HIT_BYTES <= e->hit_budget))
		c.col_to_bm25 = cost_col_to_bm25(e);
	if ((quals ? e->covered || e->host == SIFT_HYB_HOST_COL : e->agg_mode && e->covered) && (bounded || e->rows * e->sel_bm25 * HYB_HIT_BYTES <= e->hit_budget))
		c.bm25_to_col = cost_bm25_to_col(e);
	if (sift_hybrid_beats(c.col_to_bm25, &c) || sift_hybrid_beats(c.bm25_to_col, &c))
	{
		if (c.col_to_bm25 == c.bm25_to_col)
			dir = e->sel_col <= e->sel_bm25 ? SIFT_HYB_COL_TO_BM25 : SIFT_HYB_BM25_TO_COL;
		else
			dir = c.col_to_bm25 < c.bm25_to_col ? SIFT_HYB_COL_TO_BM25 : SIFT_HYB_BM25_TO_COL;
	}
	if (costs)
		*costs = c;
	return dir;
}

bool
sift_hybrid_both_apply(const SiftHybridEst *e)
{
	SiftHybridCosts c;

	sift_hybrid_choose(e, &c);
	return c.col_to_bm25 < DBL_MAX && c.bm25_to_col < DBL_MAX;
}

SiftHybridDir
sift_hybrid_pick(const SiftHybridEst *e)
{
	SiftHybridCosts c;

	sift_hybrid_choose(e, &c);
	if (c.col_to_bm25 == c.bm25_to_col)
		return e->sel_col <= e->sel_bm25 ? SIFT_HYB_COL_TO_BM25 : SIFT_HYB_BM25_TO_COL;
	return c.col_to_bm25 < c.bm25_to_col ? SIFT_HYB_COL_TO_BM25 : SIFT_HYB_BM25_TO_COL;
}

const char *
sift_hybrid_dir_name(SiftHybridDir dir)
{
	return dir == SIFT_HYB_COL_TO_BM25 ? "A" : dir == SIFT_HYB_BM25_TO_COL ? "B" : "none";
}

SiftHybridRun *
sift_hybrid_begin(MemoryContext parent, SiftHybridDir dir, const SiftHybridStore *store, Size budget_bytes, SiftHybridHeapCheck heap_check, void *check_arg)
{
	MemoryContext cx = sift_mem_allocset_create(parent, "sift hybrid");
	SiftHybridRun *r = MemoryContextAllocZero(cx, sizeof(SiftHybridRun));

	SIFT_INVARIANT(dir == SIFT_HYB_COL_TO_BM25 || dir == SIFT_HYB_BM25_TO_COL, "sift: a hybrid run has a direction");
	SIFT_INVARIANT(store->host == SIFT_HYB_HOST_TABLE || heap_check != NULL, "sift: a store on a heap needs the heap's check");
	r->dir = dir;
	r->store = store;
	r->mcxt = cx;
	r->budget = dir == SIFT_HYB_BM25_TO_COL ? Max(budget_bytes, HYB_MIN_BUDGET) : budget_bytes;
	r->heap_check = heap_check;
	r->check_arg = check_arg;
	r->stats = (SiftHybridStats) {.dir = dir,.host = store->host};
	return r;
}

void
sift_hybrid_end(SiftHybridRun *run)
{
	MemoryContextDelete(run->mcxt);
}

void
sift_hybrid_set_limit(SiftHybridRun *run, int64 limit)
{
	run->limit = limit;
}

void
sift_hybrid_set_batch_check(SiftHybridRun *run, SiftHybridBatchCheck check)
{
	run->batch_check = check;
}

static void
account(SiftHybridRun *r)
{
	const SiftTidBridge *br = r->br;

	r->stats.chunks++;
	r->stats.bridge_tids += br->ntids;
	r->stats.peak_bytes = Max(r->stats.peak_bytes, sift_tidbridge_bytes(br));
	for (uint32 i = 0; i < br->nsbs; i++)
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		{
			int			lossy = pg_popcount64(br->sbs[i].lossy_mask[w]);

			r->stats.pages_lossy += lossy;
			r->stats.pages_exact += pg_popcount64(br->sbs[i].page_mask[w]) - lossy;
		}
}

typedef struct BuildArg
{
	SiftHybridRun *run;
	bool		rowloc;
} BuildArg;

static void
build_sink(void *arg, const SiftHybridBatch *b)
{
	BuildArg   *ba = arg;
	SiftHybridRun *r = ba->run;

	r->exact_vis &= b->exact_vis;
	for (int w = 0; b->maybe != NULL && w < (b->n + 63) / 64; w++)
		r->exact_quals &= b->maybe[w] == 0;
	for (int i = 0; i < b->n; i++)
	{
		if (b->live && !(b->live[i / 64] >> (i % 64) & 1))
			continue;
		if (ba->rowloc)
			sift_tidbridge_add_rowloc(r->br, b->tid[i], (SiftTidRowLoc) {.local_g = b->local_g,.part_slot = b->part_slot,.row_ord = (uint16) i});
		else
			sift_tidbridge_add_tid(r->br, b->tid[i]);
	}
}

const SiftTidBridge *
sift_hybrid_a_bridge(SiftHybridRun *r, bool want_rowloc, bool plan_recheck)
{
	BuildArg	ba = {r, want_rowloc};
	uint32		flags = (r->store->hot_chains ? SIFT_TBR_HAS_HOT_CHAINS : 0) | (want_rowloc ? SIFT_TBR_HAS_ROWLOC : 0);

	SIFT_INVARIANT(r->dir == SIFT_HYB_COL_TO_BM25 && r->br == NULL, "sift: direction A builds its bridge once");
	r->br = sift_tidbridge_create(r->mcxt, r->budget, flags);
	r->exact_vis = true;
	r->exact_quals = r->store->exact_quals;
	r->store->scan_quals(r->store->arg, build_sink, &ba);
	sift_tidbridge_finalize(r->br);
	r->br->flags |= (r->exact_quals ? SIFT_TBR_EXACT_QUALS : 0) | (r->exact_vis ? SIFT_TBR_EXACT_VIS : 0);
	r->zero_heap = !plan_recheck && (r->br->flags & (SIFT_TBR_EXACT_QUALS | SIFT_TBR_EXACT_VIS)) == (SIFT_TBR_EXACT_QUALS | SIFT_TBR_EXACT_VIS) &&
		!(r->br->flags & (SIFT_TBR_HAS_LOSSY | SIFT_TBR_HAS_HOT_CHAINS));
	r->stats.point = sift_tidbridge_is_point(r->br);
	r->stats.empty = r->br->nsbs == 0;
	account(r);
	return r->br;
}

bool
sift_hybrid_a_accept(SiftHybridRun *r, SiftKey key)
{
	SiftTidProbe p = sift_tidbridge_probe_tid(r->br, key, NULL);

	if (p == SIFT_TIDPROBE_NO)
	{
		r->stats.bridge_rejects++;
		return false;
	}
	if (p == SIFT_TIDPROBE_LOSSY)
		r->stats.lossy_rechecks++;
	else if (r->zero_heap)
	{
		r->stats.zero_heap_rows++;
		return true;
	}
	r->stats.heap_verifies++;
	return r->heap_check(r->check_arg, key, p == SIFT_TIDPROBE_LOSSY || !(r->br->flags & SIFT_TBR_EXACT_QUALS));
}

bool
sift_hybrid_b_chunk(SiftHybridRun *r, SiftHybridHitFn next, void *arg, bool with_scores)
{
	SiftKey		key;
	float4		score;
	uint64		n = 0;

	SIFT_INVARIANT(r->dir == SIFT_HYB_BM25_TO_COL, "sift: direction B takes chunks of hits");
	if (r->br == NULL)
	{
		r->scores = with_scores;
		r->chunk_hits = r->budget / HYB_B_BYTES_PER_HIT;
		if (r->limit > 0)
			r->chunk_hits = Min(r->chunk_hits, HYB_B_FIRST_HITS(r->limit));
		r->br = sift_tidbridge_create(r->mcxt, r->budget, with_scores ? SIFT_TBR_HAS_SCORES : 0);
	}
	else
	{
		sift_tidbridge_reset(r->br);
		if (r->limit > 0 && r->stats.rows_emitted < (uint64) r->limit)
			r->chunk_hits = Min(r->chunk_hits * 4, r->budget / HYB_B_BYTES_PER_HIT);
	}
	while (n < r->chunk_hits && next(arg, &key, &score))
	{
		if (r->scores)
			sift_tidbridge_add_score(r->br, key, score);
		else
			sift_tidbridge_add_tid(r->br, key);
		n++;
	}
	if (n == 0)
		return false;
	sift_tidbridge_finalize(r->br);
	SIFT_INVARIANT(!(r->br->flags & SIFT_TBR_HAS_LOSSY), "sift: a chunk of %llu hits fits its byte budget", (unsigned long long) n);
	account(r);
	return true;
}

typedef struct EmitArg
{
	SiftHybridRun *run;
	SiftHybridSink sink;
	void	   *sink_arg;
} EmitArg;

static void
emit_sink(void *arg, const SiftHybridBatch *b)
{
	EmitArg    *ea = arg;
	SiftHybridRun *r = ea->run;
	size_t		nw = (size_t) (b->n + 63) / 64;
	uint64	   *live = palloc(nw * sizeof(uint64));
	float4	   *score = palloc(Max(b->n, 1) * sizeof(float4));
	SiftHybridBatch out = *b;
	int			kept;

	r->stats.rows_probed += b->n;
	if (b->score != NULL)		/* the scan probed the bridge before it decoded a column: live holds the rows of the bridge */
	{
		memcpy(live, b->live, nw * sizeof(uint64));
		memcpy(score, b->score, Max(b->n, 1) * sizeof(float4));
		kept = 1;
	}
	else
		kept = sift_tidbridge_probe_vec(r->br, b->tid, b->live, b->n, live, score);
	if (kept > 0)
	{
		if (!b->exact_vis && r->batch_check)
			r->batch_check(r->check_arg, b->tid, live, b->n);
		else if (!b->exact_vis)
			for (int i = 0; i < b->n; i++)
				if ((live[i / 64] >> (i % 64) & 1) && !r->heap_check(r->check_arg, b->tid[i], false))
					live[i / 64] &= ~(UINT64CONST(1) << (i % 64));
		out.live = live;
		out.score = r->scores ? score : NULL;
		for (size_t w = 0; w < nw; w++)
			r->stats.rows_emitted += pg_popcount64(live[w]);
		ea->sink(ea->sink_arg, &out);
	}
	pfree(live);
	pfree(score);
}

void
sift_hybrid_b_emit(SiftHybridRun *r, SiftHybridSink sink, void *sink_arg)
{
	EmitArg		ea = {r, sink, sink_arg};

	r->store->scan_bridge(r->store->arg, r->br, emit_sink, &ea, &r->stats);
}
