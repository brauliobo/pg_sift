/*
 * sift_search.c - a BM25 match set as the row selection of an aggregate scan (sift_search.h, docs/COL-SEARCH.md).
 *
 * Planner: takes the `key @@> q` conjuncts of one BM25 index out of the WHERE of a query SiftAgg answers and prices the match set.
 * Executor: binds the queries to the index once, then answers "the matches in superblocks [lo, hi)" with the iterator of the index
 * (the one SiftScan's unordered mode and SiftCount use), page by page in heap order, as the exact page and offset masks of a SiftFilter.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "sift_colsrc.h"
#include "sift_estimate.h"
#include "sift_eval.h"
#include "sift_planner.h"
#include "sift_search.h"

/* ---------- planner ---------- */

/* What the match set costs, in operator costs: a posting decoded and combined, a page of matches merged and copied, a term looked up */
#define SEARCH_POSTING		0.25
#define SEARCH_PAGE			12.0
#define SEARCH_TERM			400.0
#define SEARCH_BIND			20000.0	/* binding the queries: the directory, the analyzer, the dictionaries (about 60 us) */
/* A value decoded by position costs this many dense ones (docs/COL-SEARCH.md, "Measured"); beyond a fraction 1 / this of a unit's rows the unit is read whole */
#define SEARCH_SPARSE		8.0

static bool
operand_walker(Node *n, void *ctx)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan) || IsA(n, AlternativeSubPlan) || IsA(n, SubLink) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC))
		return true;
	return expression_tree_walker(n, operand_walker, ctx);
}

/* The query operand is the same at every execution of the plan: a constant, a parameter of the statement, a stable function of them */
static bool
operand_ok(Node *e)
{
	return !operand_walker(e, NULL);
}

/* The texts of the operands when all of them are constants, else NIL */
static bool
const_texts(List *queries, text ***texts, int *n)
{
	*texts = palloc(list_length(queries) * sizeof(text *));
	*n = 0;
	foreach_ptr(Node, q, queries)
	{
		if (!IsA(q, Const) || ((Const *) q)->constisnull)
			return false;
		(*texts)[(*n)++] = DatumGetTextPP(((Const *) q)->constvalue);
	}
	return true;
}

static SiftSearchPlan *
search_plan(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, List *mine, List *queries)
{
	SiftSearchPlan *sp = palloc0(sizeof(SiftSearchPlan));
	text	  **texts;
	int			n;

	sp->spec = (Node *) lcons(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(ii->indexoid), false, true), list_copy(queries));
	sp->sel = clauselist_selectivity(root, mine, rel->relid, JOIN_INNER, NULL);
	sp->docs = Max(rel->tuples * sp->sel, 1.0);
	if (const_texts(queries, &texts, &n))
	{
		SiftEstimate est;
		Relation	index = index_open(ii->indexoid, AccessShareLock);

		sift_estimate_cached(index, texts, n, &est);
		index_close(index, AccessShareLock);
		sp->postings = est.postings;
		sp->terms = est.terms;
	}
	return sp;
}

SiftSearchPlan *
sift_search_extract(PlannerInfo *root, RelOptInfo *rel, List **quals, Oid indexoid, Oid heapoid)
{
	bool		can_select = OidIsValid(indexoid) ? sift_colsrc_can_select(get_rel_relam(indexoid), true) :
		sift_colsrc_can_select(get_rel_relam(heapoid), false);

	if (!can_select)
		return NULL;
	foreach_ptr(IndexOptInfo, ii, rel->indexlist)
	{
		List	   *mine = NIL,
				   *rest = NIL,
				   *queries = NIL;

		if (ii->relam != sift_catalog()->am || ii->ncolumns != 1 || ii->hypothetical || (ii->indpred != NIL && !ii->predOK))
			continue;
		foreach_ptr(Node, q, *quals)
		{
			Expr	   *query;

			if (sift_match_clause(q, ii, &query) && operand_ok((Node *) query))
			{
				mine = lappend(mine, q);
				queries = lappend(queries, query);
			}
			else
				rest = lappend(rest, q);
		}
		if (mine == NIL)
			continue;
		*quals = rest;
		return search_plan(root, rel, ii, mine, queries);
	}
	return NULL;
}

double
sift_search_cost(const SiftSearchPlan *sp, double nrows, double nunits, double *rows, double *units)
{
	double		docs = Min(sp->docs, nrows),
				postings = sp->postings > 0 ? sp->postings : docs * 2,
				pages = Min(docs, Max(nrows / 100.0, 1.0));

	/* the units with a match: all of them but for a rare query */
	*units = Max(nunits * (1.0 - exp(-docs / Max(nunits, 1.0))), Min(docs, 1.0));
	/* the rows read: the matches by position, or the whole unit when they are many */
	*rows = Min(nrows * (*units / Max(nunits, 1.0)), docs * SEARCH_SPARSE);
	return SEARCH_BIND + sp->terms * SEARCH_TERM + postings * SEARCH_POSTING + pages * SEARCH_PAGE;
}

/* "orders_title @@> 'a' AND ..." */
char *
sift_search_describe(Node *spec)
{
	List	   *l = (List *) spec;
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = 1; i < list_length(l); i++)
		appendStringInfo(&buf, "%s%s @@> %s", i > 1 ? " AND " : "", get_rel_name(DatumGetObjectId(((Const *) linitial(l))->constvalue)),
						 deparse_expression(list_nth(l, i), NIL, false, false));
	return buf.data;
}

/* ---------- executor ---------- */

int			sift_search_chunk_sbs = 32;

void
sift_search_init(void)
{
	DefineCustomIntVariable("sift.test_search_chunk_sbs", "Superblocks whose BM25 matches an aggregate scan holds at a time (test only).", NULL,
							&sift_search_chunk_sbs, 32, 1, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
}

struct SiftSearch
{
	MemoryContext cxt;
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	SiftIndexState *st;
	SiftPlan   *plan;			/* NULL: a query is NULL, nothing matches */
	SiftMatchIter *it;
	SiftFilter *filter;
	uint32		want_lo,
				want_hi,
				done_hi;
	bool		served;
	/* the phrases the index leaves to the text of the row */
	bool		heap_ready;
	SiftVis		vis;
	TupleTableSlot *slot;
	EState	   *estate;
	IndexInfo  *iinfo;
	MemoryContext tmpcx;
	int64		matches;
};

static bool
next_range(void *arg, uint32 *lo, uint32 *hi)
{
	SiftSearch *s = arg;

	if (s->served)
		return false;
	*lo = s->want_lo;
	*hi = s->want_hi;
	return s->served = true;
}

static void
bind_queries(SiftSearch *s, List *operands, ExprContext *econtext, PlanState *parent)
{
	int			n = list_length(operands),
				i = 0;
	SiftQuery **queries = palloc(n * sizeof(SiftQuery *));

	foreach_ptr(Node, e, operands)
	{
		bool		isnull;
		ExprState  *es = ExecInitExpr((Expr *) e, parent);
		Datum		d = ExecEvalExprSwitchContext(es, econtext, &isnull);
		text	   *q;

		if (isnull)
			return;
		q = DatumGetTextPP(d);
		queries[i++] = sift_state_query(s->st, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
	}
	s->plan = sift_plan_bind(s->st, queries, n, 0);
}

SiftSearch *
sift_search_open(Node *spec, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt)
{
	List	   *l = (List *) spec;
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftSearch *s = palloc0(sizeof(SiftSearch));

	s->cxt = cxt;
	s->heap = heap;
	s->snapshot = snapshot;
	s->index = index_open(DatumGetObjectId(((Const *) linitial(l))->constvalue), AccessShareLock);
	s->st = sift_state_open(s->index, cxt);
	s->filter = sift_filter_create(cxt);
	bind_queries(s, list_delete_first(list_copy(l)), econtext, parent);
	MemoryContextSwitchTo(old);
	return s;
}

void
sift_search_close(SiftSearch *s)
{
	if (s->plan && s->it)
		sift_iter_end(s->it);
	if (s->heap_ready)
	{
		sift_vis_end(&s->vis);
		ExecDropSingleTupleTableSlot(s->slot);
		FreeExecutorState(s->estate);
	}
	sift_state_close(s->st);
	index_close(s->index, NoLock);
}

int64
sift_search_matches(const SiftSearch *s)
{
	return s->matches;
}

const char *
sift_search_name(const SiftSearch *s)
{
	return RelationGetRelationName(s->index);
}

/* ---- rows the index leaves to their text ---- */

static void
heap_setup(SiftSearch *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	sift_vis_begin(&s->vis, s->heap, s->snapshot);
	s->slot = table_slot_create(s->heap, NULL);
	s->estate = CreateExecutorState();
	s->iinfo = BuildIndexInfo(s->index);
	s->tmpcx = AllocSetContextCreate(s->cxt, "sift search recheck", ALLOCSET_DEFAULT_SIZES);
	s->heap_ready = true;
	MemoryContextSwitchTo(old);
}

/* The indexed text of the row at tid decides: the visible version's (the same along a HOT chain) */
static bool
text_matches(SiftSearch *s, BlockNumber blk, int off)
{
	ItemPointerData tid;
	Datum		value;
	bool		isnull,
				ok;
	MemoryContext old = MemoryContextSwitchTo(s->tmpcx);

	ItemPointerSet(&tid, blk, off);
	ok = sift_vis_fetch(&s->vis, &tid, s->slot);
	if (ok)
	{
		ResetPerTupleExprContext(s->estate);
		GetPerTupleExprContext(s->estate)->ecxt_scantuple = s->slot;
		FormIndexDatum(s->iinfo, s->slot, s->estate, &value, &isnull);
		ok = !isnull && sift_plan_recheck(s->plan, DatumGetTextPP(value));
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(s->tmpcx);
	return ok;
}

/* Keeps in the page the candidates whose text matches (the rows `sure` already decided stay); false when none is left */
static bool
recheck_page(SiftSearch *s, SiftPageMatch *pm)
{
	bool		any = false;

	if (!s->heap_ready)
		heap_setup(s);
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
	{
		for (uint64 m = pm->offmask[wd] & ~pm->sure[wd]; m; m &= m - 1)
			if (!text_matches(s, pm->blk, wd * 64 + pg_rightmost_one_pos64(m) + 1))
				pm->offmask[wd] &= ~(m & -m);
		any |= pm->offmask[wd] != 0;
	}
	return any;
}

const SiftFilter *
sift_search_range(SiftSearch *s, uint32 lo, uint32 hi)
{
	SiftPageMatch pm;

	sift_filter_reset(s->filter);
	if (s->plan == NULL)
		return s->filter;
	if (s->it == NULL || lo < s->done_hi)
	{
		if (s->it)
			sift_iter_end(s->it);
		s->it = sift_iter_begin(s->plan);
		sift_iter_ranges(s->it, next_range, s);
	}
	s->want_lo = lo;
	s->want_hi = hi;
	s->served = false;
	while (sift_iter_next(s->it, &pm))
	{
		CHECK_FOR_INTERRUPTS();
		if (pm.recheck && !recheck_page(s, &pm))
			continue;
		sift_filter_add_page(s->filter, pm.blk, pm.offmask);
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			s->matches += pg_popcount64(pm.offmask[k]);
	}
	s->done_hi = hi;
	return s->filter;
}
