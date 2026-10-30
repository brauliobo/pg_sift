/*
 * agg/exec/topn.c - the top-N scan of a SiftAgg node
 * The zones, gate conditions, runs and widening the node passes to the top-N reader, and the
 * rows it hands out.
 */
#include "postgres.h"

#include "optimizer/optimizer.h"

#include "col/gorder.h"
#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/exec/int.h"

/*
 * The match set of the superblocks a window is in. The first window is asked for alone (the scan often ends in it, and a match set costs by
 * the superblocks it covers); the chunk around each later one holds the windows that follow it, which a backwards visit cannot get cheaply:
 * the match iterator restarts when asked for superblocks it has passed.
 */
typedef struct TopnChunk
{
	uint32		lo,
				hi;
	const SiftFilter *flt;
} TopnChunk;

/* The zone of the first sort leaf over the units of superblock sb, from the segment headers (the unit is not read); false when it has none */
static bool
topn_zone(AggExec *ex, uint32 sb, SiftSrcZone *z)
{
	return sift_colsrc_zone_range(ex->src, sb, sb + 1, sift_topn_leaf(ex->topn, 0), z) > 0;
}

/*
 * Does a unit of the superblocks [lo, hi) have a zone that can hold a better row than the n-th so far? The new version of an updated row is
 * stored wherever there is room (a page with free space in a heap, the write buffer of a table) whatever the value of the sort column, so
 * the superblocks do not follow the column even when the zones of a few of them say so, and no superblock can be taken for the last that
 * matters: every window is decided by the zones of its own units (read together from the segment headers, no unit is opened), and the match
 * set is built only for a window that has one.
 */
static bool
topn_window_viable(AggExec *ex, uint32 lo, uint32 hi)
{
	SiftSrcZone z;
	uint32		n = sift_colsrc_zone_range(ex->src, lo, hi, sift_topn_leaf(ex->topn, 0), &z);

	if (sift_topn_zone_viable(ex->topn, &z))
		return true;
	while (n-- > 0)
		sift_topn_skip(ex->topn);
	return false;
}

/* The zones of the leading sort keys of superblock sb, which bound the walk (agg/topn.h); its number of units */
static uint32
topn_walk_zones(AggExec *ex, uint32 sb, SiftSrcZone *zs)
{
	uint32		units = 0;

	for (int j = 0; j < sift_topn_walk_keys(ex->topn); j++)
		units = sift_colsrc_zone_range(ex->src, sb, sb + 1, sift_topn_leaf(ex->topn, j), &zs[j]);
	return units;
}

/* Can a unit of superblock sb of the walk hold a row that sorts before the n-th so far, or ties with it? */
static bool
topn_walk_viable(AggExec *ex, uint32 sb)
{
	SiftSrcZone zs[SIFT_TOPN_MAX_KEYS];
	uint32		n = topn_walk_zones(ex, sb, zs);

	if (sift_topn_walk_zone_viable(ex->topn, zs))
		return true;
	while (n-- > 0)
		sift_topn_skip(ex->topn);
	return false;
}

static void
topn_scan(AggExec *ex, uint32 lo, uint32 hi, uint32 nsb, TopnChunk *ch)
{
	if (ex->search)
	{
		if (ch->flt == NULL || lo < ch->lo || hi > ch->hi)
		{
			uint32		width = ch->flt == NULL ? hi - lo : Max(hi - lo, sift_search_chunk_sbs);

			ch->lo = sift_topn_reverse(ex->topn) ? (hi > width ? hi - width : 0) : lo;
			ch->hi = sift_topn_reverse(ex->topn) ? hi : Min(lo + width, nsb);
			/* units that are not superblocks of the heap hold rows of any block: the match set is the whole table's, made once */
			if (!ex->sb_heap)
			{
				ch->lo = 0;
				ch->hi = nsb;
			}
			ch->flt = sift_search_range(ex->search, ch->lo, ch->hi);
		}
		sift_colsrc_select(ex->src, ch->flt);
	}
	scan_units(ex, lo, hi);
}

/*
 * The items of a parallel top-N, taken from the shared counter by the leader and the workers: a window of superblocks of the leader's visit
 * order, left out when the zones of its units cannot beat the n-th row of any participant (the bound they share, agg/topn.h)
 */
void
topn_items(AggExec *ex)
{
	ScanShared *sh = ex->shared;
	TopnChunk	ch = {0, 0, NULL};
	uint32		item;

	while (take_item(ex, &item))
	{
		uint32		lo,
					hi;

		if (sh->topn_walk)
		{
			lo = shared_order(sh, ex->spec)[item];
			hi = lo + 1;
			if (!topn_walk_viable(ex, lo))
			{
				/* the walk gives the best bound first: the superblocks after this one are no better, whatever bound the others reach */
				pg_atomic_write_u32(&sh->next, sh->nitems);
				break;
			}
		}
		else
		{
			if (sh->topn_reverse)
			{
				hi = sh->nsb - item * sh->topn_item;
				lo = hi > sh->topn_item ? hi - sh->topn_item : 0;
			}
			else
			{
				lo = item * sh->topn_item;
				hi = Min(lo + sh->topn_item, sh->nsb);
			}
			if (sift_topn_wants_zones(ex->topn) && !topn_window_viable(ex, lo, hi))
				continue;
		}
		topn_scan(ex, lo, hi, sh->nsb, &ch);
	}
}

/* The workers and the leader take the items; the heaps of the workers go into the leader's, which is the result as one process finds it */
static void
topn_parallel(AggExec *ex)
{
	par_launch(ex);
	if (ex->shared == NULL)
	{
		ex->shared = MemoryContextAlloc(ex->cxt, shared_size(ex->spec, ex->nvorder));
		shared_init(ex->shared, ex);
	}
	sift_topn_share(ex->topn, &ex->shared->topn_bound);
	if (ex->par == NULL || parallel_leader_participation)
		topn_items(ex);
	if (ex->par)
		par_finish(ex);
	sift_topn_finish(ex->topn);
}

/* a scan with no join or aggregate, whatever its conditions */
static bool
topn_plain_ok(AggExec *ex)
{
	return sift_aggspec_plain(ex->spec) && ex->ntype == 0 && ex->nwide == 0 && !ex->recheck && ex->be == NULL && ex->tid_sb_lo == 0 &&
		ex->tid_sb_hi == PG_UINT32_MAX;
}

/* a scan with no condition, join or aggregate: every row of a clean unit passes, and the head of its sorted run names the n best of them */
static bool
topn_heads_ok(AggExec *ex)
{
	return ex->spec->npred == 0 && ex->nzone == 0 && ex->nfilter == 0 && topn_plain_ok(ex);
}

/*
 * The condition of the scan as the heap row is tested against it, when it is all comparisons of integer, date or time columns with constants of their type (an
 * operand that is NULL is no row: the units answer it). A column that is not the first sort key is tested on the heap row; the scan accepts it only where the first
 * is pinned (an equality in the same conjunction: the page after a key) or an array grouped by the column answers it (sift_topn_gorder), or it would read the array from the
 * bound to the end for rows that few of them pass; `pinned` says the first is.
 */
static SiftTopGPred *
topn_gpred(AggExec *ex, const SiftAggPred *p, int keyleaf, bool pinned)
{
	SiftTopGPred *g = palloc0(sizeof(SiftTopGPred));

	if (p->kind == SAP_AND || p->kind == SAP_OR)
	{
		g->kind = p->kind == SAP_AND ? STG_AND : STG_OR;
		g->nchild = p->nchild;
		g->child = palloc(p->nchild * sizeof(SiftTopGPred *));
		for (int i = 0; p->kind == SAP_AND && i < p->nchild && !pinned; i++)
			pinned = p->child[i]->kind == SAP_CMP && p->child[i]->leaf == keyleaf && p->child[i]->op == SCMP_EQ;
		for (int i = 0; i < p->nchild; i++)
			if ((g->child[i] = topn_gpred(ex, p->child[i], keyleaf, pinned)) == NULL)
				return NULL;
		return g;
	}
	if (p->kind == SAP_NULL && p->op == 1 && p->level == 0 && !p->late && p->leaf < ex->nbase && (g->attno = sift_topn_leaf_attno(ex->lay, p->leaf)) > 0)
	{
		g->kind = STG_NOTNULL;
		g->lead = p->leaf == keyleaf;
		g->pinned = pinned;
		g->leaf = p->leaf;
		g->typid = ex->lay->leaf[p->leaf].typid;
		return g;
	}
	if (p->kind == SAP_LUT && p->level == 0 && !p->late && p->leaf == keyleaf && p->leaf < ex->nbase && (g->attno = sift_topn_text_attno(ex->lay, p->leaf)) > 0)
	{
		SiftSrcStrTest st = {0};

		if (!sift_aggspec_text_bound(p, ex->lay, ex->nbase, &st))
			return NULL;
		g->kind = STG_STR;
		g->sop = st.op;
		g->str = st.vals[0];
		g->slen = st.lens[0];
		g->collid = st.collid;
		g->lead = true;
		g->pinned = pinned;
		g->leaf = p->leaf;
		g->typid = ex->lay->leaf[p->leaf].typid;
		return g;
	}
	/* leaf IN (constants) is an OR of equalities (as many as a global order grouped by the leaf reads at once); NOT IN and a NULL element are for the units, and so is the set of keys of a join, which has no expression */
	if (p->kind == SAP_IN && p->op == 0 && p->expr != NULL && p->level == 0 && !p->late && p->leaf < ex->nbase &&
		(g->attno = sift_topn_leaf_attno(ex->lay, p->leaf)) > 0 && sift_topn_same_scale(p->argtype, ex->lay->leaf[p->leaf].typid))
	{
		bool		isnull;
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);
		ExprState  *e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
		Datum		d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
		ArrayType  *arr;
		int16		elen;
		bool		ebyval;
		char		ealign;
		Datum	   *elems;
		bool	   *nulls;
		int			n;

		MemoryContextSwitchTo(old);
		if (isnull)
			return NULL;
		arr = DatumGetArrayTypeP(d);
		get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elen, &ebyval, &ealign);
		deconstruct_array(arr, ARR_ELEMTYPE(arr), elen, ebyval, ealign, &elems, &nulls, &n);
		if (n == 0 || n > SIFT_GORD_PIN_MAX)
			return NULL;
		g->kind = STG_OR;
		g->nchild = n;
		g->child = palloc(n * sizeof(SiftTopGPred *));
		for (int i = 0; i < n; i++)
		{
			SiftTopGPred *c = palloc0(sizeof(SiftTopGPred));

			if (nulls[i])
				return NULL;
			c->kind = STG_CMP;
			c->op = SCMP_EQ;
			c->lead = p->leaf == keyleaf;
			c->pinned = pinned;
			c->leaf = p->leaf;
			c->attno = g->attno;
			c->typid = ex->lay->leaf[p->leaf].typid;
			c->val = sift_aggspec_operand_int(p->argtype, elems[i]);
			g->child[i] = c;
		}
		return g;
	}
	if (p->kind == SAP_CMP && p->level == 0 && !p->late && p->leaf < ex->nbase &&
		(g->attno = sift_topn_leaf_attno(ex->lay, p->leaf)) > 0 && sift_topn_same_scale(p->argtype, ex->lay->leaf[p->leaf].typid))
	{
		bool		isnull;
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);
		ExprState  *e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
		Datum		d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);

		MemoryContextSwitchTo(old);
		if (isnull)
			return NULL;
		if (p->calmode)
			d = sift_cal_bound(p->cal, p->calmode, d);
		g->kind = STG_CMP;
		g->op = p->op;
		g->lead = p->leaf == keyleaf;
		g->pinned = pinned;
		g->leaf = p->leaf;
		g->typid = ex->lay->leaf[p->leaf].typid;
		g->val = sift_aggspec_operand_int(p->argtype, d);
		return g;
	}
	return NULL;
}

/* all conjuncts of the scan as one condition: NULL when the order of the key cannot answer one */
static SiftTopGPred *
topn_gcond(AggExec *ex, const SiftTopN *t)
{
	SiftTopGPred *g = palloc0(sizeof(SiftTopGPred));
	int			keyleaf = sift_topn_leaf(t, 0);
	bool		pinned = false;

	g->kind = STG_AND;
	g->nchild = ex->spec->npred;
	g->child = palloc(g->nchild * sizeof(SiftTopGPred *));
	for (int i = 0; i < g->nchild; i++)
		pinned |= ex->spec->pred[i]->kind == SAP_CMP && ex->spec->pred[i]->leaf == keyleaf && ex->spec->pred[i]->op == SCMP_EQ;
	for (int i = 0; i < g->nchild; i++)
		if ((g->child[i] = topn_gpred(ex, ex->spec->pred[i], keyleaf, pinned)) == NULL)
			return NULL;
	return g;
}

/* the conjuncts of the scan that the order of the key can read, as one condition (the others are left to the mask of the scan, which tests them all): NULL when none */
static SiftTopGPred *
topn_gbox(AggExec *ex, const SiftTopN *t)
{
	SiftTopGPred *g = palloc0(sizeof(SiftTopGPred));
	int			keyleaf = sift_topn_leaf(t, 0);

	g->kind = STG_AND;
	g->child = palloc(ex->spec->npred * sizeof(SiftTopGPred *));
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftTopGPred *c = topn_gpred(ex, ex->spec->pred[i], keyleaf, false);

		if (c != NULL)
			g->child[g->nchild++] = c;
	}
	return g->nchild > 0 ? g : NULL;
}

/* the vectors of a scan are allocated when the first unit is read: a walk that the heads of the runs answer reads none */
static void
topn_buffers(AggExec *ex, bool *done)
{
	if (!*done)
		scan_buffers(ex);
	*done = true;
}

static void
topn_run(AggExec *ex)
{
	SiftTopN   *t;
	uint32		nsb,
				lo,
				hi,
				best;
	SiftSrcZone z;
	TopnChunk	ch = {0, 0, NULL};
	SiftTopGPred *cond = NULL;
	bool		parallel,
				heads,
				bufs = false;

	ex->started = true;
	engine_open(ex, ex->snap, ex->params);
	agg_test_wait();
	if (!sift_topn_heads || !sift_topn_walk || ex->spec->naggs > 0 || ex->search != NULL || !topn_heads_ok(ex))
		topn_buffers(ex, &bufs);
	table_init(ex);
	t = ex->topn;
	if (sift_topn_empty(t))
		return;
	if (ex->spec->npred > 0)
	{
		cond = topn_gcond(ex, t);
		sift_topn_key_range(t, topn_gbox(ex, t));
		/* the filters of the scan are the conditions the top-N tests (the rows of a group or a recheck go through the filters) */
		if (ex->spec->naggs == 0 && ex->spec->group < 0 && !ex->recheck && ex->search == NULL)
			sift_topn_lazy(t, ex->lay, cond);
	}
	if (topn_plain_ok(ex) && ex->spec->nworkers == 0 && sift_topn_gorder_on)
	{
		if ((ex->spec->npred == 0 || cond != NULL) && sift_topn_gorder(t, ex->src, cond))
			return;
	}
	nsb = sift_colsrc_nsb(ex->src);
	if (sift_topn_wants_zones(t) && nsb >= 2 * sift_topn_nprobes())
	{
		/* the begin loads the write buffers, which the zones of the walk and the head bypass need to know are empty (sift_col_walk_stamp) */
		if (ex->search)
			sift_colsrc_select(ex->src, NULL);	/* the units as they are, not those of the last match set */
		sift_colsrc_begin(ex->src, 0, 1, NULL, 0);
		for (int i = 0; i < sift_topn_nprobes(); i++)
		{
			uint32		sb = sift_topn_probe_sb(t, i, nsb);

			if (topn_zone(ex, sb, &z))
				sift_topn_probe(t, sb, &z);
		}
	}
	sift_topn_start(t, nsb);
	parallel = ex->spec->nworkers > 0 && sift_topn_parallel_ok(t);
	if (sift_topn_walk && ex->search == NULL && sift_topn_wants_zones(t) && !sift_topn_follows(t) && nsb >= 2 * sift_topn_nprobes())
	{
		/* the match set of a search restarts when it is asked for superblocks it has passed: only the scans without one are walked */
		sift_topn_walk_setup(t);
		if (parallel)
		{
			topn_buffers(ex, &bufs);
			ex->vorder = (uint32 *) sift_topn_walk_order(t);
			ex->nvorder = nsb;
			topn_parallel(ex);
			return;
		}
		sift_topn_run_budget(t, nsb);
		/* a scan with no condition, join or aggregate: every row of a clean unit passes, and the head of its sorted run names the n best of them */
		heads = topn_heads_ok(ex);
		while (sift_topn_walk_next(t, &lo))
			if (!heads || !sift_topn_sb_head(t, ex->src, lo))
			{
				topn_buffers(ex, &bufs);
				scan_units(ex, lo, lo + 1);
			}
		while (sift_topn_revisit_next(t, &lo))
		{
			topn_buffers(ex, &bufs);
			scan_units(ex, lo, lo + 1);
		}
		sift_topn_finish(t);
		return;
	}
	topn_buffers(ex, &bufs);
	if (parallel)
	{
		topn_parallel(ex);
		return;
	}
	while (sift_topn_next_window(t, &lo, &hi, &best))
	{
		if (sift_topn_wants_zones(t) && !topn_window_viable(ex, lo, hi))
			continue;
		if (sift_topn_reverse(t) && hi - lo > 1 && ex->search == NULL && sift_topn_wants_zones(t))
		{
			/* the best superblock of a window is its last: one by one from there, the zone of each against the bar the better ones set, not all of them read in table order */
			for (uint32 sb = hi; sb-- > lo;)
				if (topn_window_viable(ex, sb, sb + 1))
					topn_scan(ex, sb, sb + 1, nsb, &ch);
			continue;
		}
		topn_scan(ex, lo, hi, nsb, &ch);
	}
	sift_topn_finish(t);
}

/*
 * TOPN_TIES: the last row of the heap ties with the one that counts on the keys the scan holds, so rows of the tie that the Sort above
 * needs may be missing. The scan starts again on an engine of its own with a heap 16 times larger (twice, then every row is kept).
 */
static void
topn_widen(AggExec *ex)
{
	EState	   *estate = ex->css.ss.ps.state;

	while (sift_topn_incomplete(ex->topn))
	{
		AggExec    *w = MemoryContextAllocZero(estate->es_query_cxt, sizeof(AggExec));

		w->spec = ex->spec;
		w->pack = ex->pack;
		w->params = ex->params;
		w->snap = ex->snap;
		w->css.ss.ps.state = estate;
		w->css.custom_ps = ex->css.custom_ps;
		w->widen = ex->widen + 1;
		engine_contexts(w, estate->es_query_cxt);
		engine_rels(w);
		topn_run(w);
		sift_topn_close(ex->topn);
		ex->topn = w->topn;
		w->topn = NULL;
		ex->widen = w->widen;
		ex->retries = lappend(ex->retries, w);
	}
}

/* After the rows of the heap, the aggregates over every row that passed (the one group of the scan) are the same in all of them */
TupleTableSlot *
topn_next(AggExec *ex, TupleTableSlot *slot)
{
	SiftAggSpec *s = ex->spec;
	int			nout = slot->tts_tupleDescriptor->natts - s->naggs;

	if (!ex->started)
	{
		topn_run(ex);
		topn_widen(ex);
		if (s->naggs > 0 && !sift_topn_empty(ex->topn))
		{
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

			finish_groups(ex);
			ex->total_val = palloc(s->naggs * sizeof(Datum));
			ex->total_null = palloc(s->naggs * sizeof(bool));
			for (int i = 0; i < s->naggs; i++)
				ex->total_val[i] = agg_datum(ex, i, ex->glist[0], &ex->total_null[i]);
			MemoryContextSwitchTo(old);
		}
	}
	if (sift_topn_empty(ex->topn))
		return NULL;
	/* before the row: it materializes the slot, which copies every value of the tuple */
	for (int i = 0; i < s->naggs; i++)
	{
		slot->tts_values[nout + i] = ex->total_val[i];
		slot->tts_isnull[nout + i] = ex->total_null[i];
	}
	return sift_topn_next(ex->topn, slot) ? slot : NULL;
}
