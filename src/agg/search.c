/*
 * agg/search.c - a BM25 match set as the row selection of an aggregate scan (agg/search.h, docs/COL-SEARCH.md).
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

#include "agg/colsrc.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "bm25/planner.h"
#include "agg/search.h"

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
bool
sift_operand_ok(Node *e)
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

static bool
estimate_arm(Relation index, List *arm, SiftEstimate *est)
{
	text	  **texts;
	int			n;

	if (!const_texts(arm, &texts, &n))
		return false;
	sift_estimate_cached(index, texts, n, est);
	return true;
}

/* The postings and terms the arms read when every operand is a constant, else both stay 0 (not known) */
static void
estimate_arms(Oid indexoid, List *arms, SiftSearchPlan *sp)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	double		postings = 0,
				terms = 0;
	bool		known = true;

	foreach_node(List, arm, arms)
	{
		SiftEstimate est;

		if (!(known = estimate_arm(index, arm, &est)))
			break;
		postings += est.postings;
		terms += est.terms;
	}
	index_close(index, AccessShareLock);
	if (known)
	{
		sp->postings = postings;
		sp->terms = terms;
	}
}

/* arms: the operand lists of the arms, ORed; the operands of an arm are ANDed. mine: the clauses of the WHERE they stand for */
static SiftSearchPlan *
search_plan(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, List *mine, List *arms)
{
	SiftSearchPlan *sp = palloc0(sizeof(SiftSearchPlan));

	sp->spec = (Node *) lcons(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(ii->indexoid), false, true), list_copy(arms));
	sp->sel = clauselist_selectivity(root, mine, rel->relid, JOIN_INNER, NULL);
	sp->docs = Max(rel->tuples * sp->sel, 1.0);
	estimate_arms(ii->indexoid, arms, sp);
	return sp;
}

/* The arms of `key @@> a OR key @@> b ...` of the index, one operand each; NIL when a branch is anything else */
static List *
or_arms(Node *q, IndexOptInfo *ii)
{
	List	   *arms = NIL;

	if (!IsA(q, BoolExpr) || ((BoolExpr *) q)->boolop != OR_EXPR)
		return NIL;
	foreach_ptr(Node, branch, ((BoolExpr *) q)->args)
	{
		Expr	   *query;

		if (!sift_match_clause(branch, ii, &query) || !sift_operand_ok((Node *) query))
			return NIL;
		arms = lappend(arms, list_make1(query));
	}
	return arms;
}

SiftSearchPlan *
sift_search_extract(PlannerInfo *root, RelOptInfo *rel, List **quals, Oid indexoid, Oid heapoid, bool allow_or)
{
	bool		can_select = OidIsValid(indexoid) ? sift_colsrc_can_select(get_rel_relam(indexoid), true) :
		sift_colsrc_can_select(get_rel_relam(heapoid), false);

	if (!can_select)
		return NULL;
	foreach_ptr(IndexOptInfo, ii, rel->indexlist)
	{
		List	   *mine = NIL,
				   *rest = NIL,
				   *queries = NIL,
				   *arms = NIL;
		Node	   *orq = NULL;

		if (ii->relam != sift_catalog()->am || ii->ncolumns != 1 || ii->hypothetical || (ii->indpred != NIL && !ii->predOK))
			continue;
		foreach_ptr(Node, q, *quals)
		{
			Expr	   *query;

			if (sift_match_clause(q, ii, &query) && sift_operand_ok((Node *) query))
			{
				mine = lappend(mine, q);
				queries = lappend(queries, query);
				continue;
			}
			rest = lappend(rest, q);
			if (allow_or && orq == NULL && (arms = or_arms(q, ii)) != NIL)
				orq = q;
		}
		if (mine == NIL && orq == NULL)
			continue;
		/* an OR group stands alone: next to plain conjuncts it stays a condition of the scan, which the scan cannot evaluate */
		*quals = mine != NIL ? rest : list_delete_ptr(rest, orq);
		return mine != NIL ? search_plan(root, rel, ii, mine, list_make1(queries)) : search_plan(root, rel, ii, list_make1(orq), arms);
	}
	return NULL;
}

/* The cost of opening the match set, whatever part of the table the scan reads: the iterator takes the posting lists of the terms whole */
double
sift_search_open_cost(const SiftSearchPlan *sp, double nrows)
{
	double		docs = Min(sp->docs, nrows),
				postings = sp->postings > 0 ? sp->postings : docs * 2;

	return SEARCH_BIND + sp->terms * SEARCH_TERM + postings * SEARCH_POSTING;
}

double
sift_search_cost(const SiftSearchPlan *sp, double nrows, double nunits, double *rows, double *units)
{
	double		docs = Min(sp->docs, nrows),
				pages = Min(docs, Max(nrows / 100.0, 1.0));

	/* the units with a match: all of them but for a rare query */
	*units = Max(nunits * (1.0 - exp(-docs / Max(nunits, 1.0))), Min(docs, 1.0));
	/* the rows read: the matches by position, or the whole unit when they are many */
	*rows = Min(nrows * (*units / Max(nunits, 1.0)), docs * SEARCH_SPARSE);
	return sift_search_open_cost(sp, nrows) + pages * SEARCH_PAGE;
}

/* "orders_title @@> 'a' AND orders_title @@> 'b' OR orders_title @@> 'c'": the operands of an arm ANDed, the arms ORed */
char *
sift_search_describe(Node *spec)
{
	List	   *l = (List *) spec;
	const char *name = get_rel_name(DatumGetObjectId(((Const *) linitial(l))->constvalue));
	StringInfoData buf;

	initStringInfo(&buf);
	for (int a = 1; a < list_length(l); a++)
	{
		appendStringInfoString(&buf, a > 1 ? " OR " : "");
		foreach_ptr(Node, q, list_nth_node(List, l, a))
			appendStringInfo(&buf, "%s%s @@> %s", foreach_current_index(q) ? " AND " : "", name, deparse_expression(q, NIL, false, false));
	}
	return buf.data;
}

int			sift_search_chunk_sbs = 32;

void
sift_search_init(void)
{
	DefineCustomIntVariable("sift.test_search_chunk_sbs", "Superblocks whose BM25 matches an aggregate scan holds at a time (test only).", NULL,
							&sift_search_chunk_sbs, 32, 1, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
}

struct SiftSearch;

/* One arm of the search: the AND of its queries. The match set is the union of the arms' */
typedef struct SearchArm
{
	struct SiftSearch *s;
	SiftPlan   *plan;			/* NULL: a query is NULL, nothing matches */
	SiftMatchIter *it;
	SiftPageMatch pm;			/* the arm's next page, when has */
	bool		has;
	bool		served;
} SearchArm;

struct SiftSearch
{
	MemoryContext cxt;
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	SiftIndexState *st;
	int			narms;
	SearchArm  *arm;
	SiftFilter *filter;
	uint32		want_lo,
				want_hi,
				done_hi;
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
	SearchArm  *a = arg;

	if (a->served)
		return false;
	*lo = a->s->want_lo;
	*hi = a->s->want_hi;
	return a->served = true;
}

static void
bind_queries(SearchArm *a, List *operands, ExprContext *econtext, PlanState *parent)
{
	SiftSearch *s = a->s;
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
	a->plan = sift_plan_bind(s->st, queries, n, 0);
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
	s->narms = list_length(l) - 1;
	s->arm = palloc0(s->narms * sizeof(SearchArm));
	for (int i = 0; i < s->narms; i++)
	{
		s->arm[i].s = s;
		bind_queries(&s->arm[i], list_nth_node(List, l, i + 1), econtext, parent);
	}
	MemoryContextSwitchTo(old);
	return s;
}

void
sift_search_close(SiftSearch *s)
{
	for (int i = 0; i < s->narms; i++)
		if (s->arm[i].it)
			sift_iter_end(s->arm[i].it);
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

/* ---- rows the index leaves to their text ---- */

static void
heap_setup(SiftSearch *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	sift_vis_begin(&s->vis, s->heap, s->snapshot, NULL, 0);
	s->slot = table_slot_create(s->heap, NULL);
	s->estate = CreateExecutorState();
	s->iinfo = BuildIndexInfo(s->index);
	s->tmpcx = AllocSetContextCreate(s->cxt, "sift search recheck", ALLOCSET_DEFAULT_SIZES);
	s->heap_ready = true;
	MemoryContextSwitchTo(old);
}

/* The indexed text of the row at tid decides: the visible version's (the same along a HOT chain) */
static bool
text_matches(SiftSearch *s, SiftPlan *plan, BlockNumber blk, int off)
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
		ok = !isnull && sift_plan_recheck(plan, DatumGetTextPP(value));
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(s->tmpcx);
	return ok;
}

/* Keeps in the page the candidates whose text matches (the rows `sure` already decided stay); false when none is left */
static bool
recheck_page(SiftSearch *s, SiftPlan *plan, SiftPageMatch *pm)
{
	bool		any = false;

	if (!s->heap_ready)
		heap_setup(s);
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
	{
		for (uint64 m = pm->offmask[wd] & ~pm->sure[wd]; m; m &= m - 1)
			if (!text_matches(s, plan, pm->blk, wd * 64 + pg_rightmost_one_pos64(m) + 1))
				pm->offmask[wd] &= ~(m & -m);
		any |= pm->offmask[wd] != 0;
	}
	return any;
}

/* The arm's next page that its queries decide, the text of the rows they leave undecided included */
static void
arm_next(SearchArm *a)
{
	a->has = false;
	while (!a->has && sift_iter_next(a->it, &a->pm))
	{
		CHECK_FOR_INTERRUPTS();
		a->has = !a->pm.recheck || recheck_page(a->s, a->plan, &a->pm);
	}
}

/* The arms look at the range from its start: a new iterator when they have passed it */
static void
arm_start(SearchArm *a)
{
	if (a->plan == NULL)
		return;
	if (a->it == NULL || a->s->want_lo < a->s->done_hi)
	{
		if (a->it)
			sift_iter_end(a->it);
		a->it = sift_iter_begin(a->plan);
		sift_iter_ranges(a->it, next_range, a);
	}
	a->served = false;
	arm_next(a);
}

/* The next page of the union of the arms in heap order: the offsets that any arm having the page matches */
static bool
union_page(SiftSearch *s, BlockNumber *blk, uint64 *mask)
{
	bool		found = false;

	for (int i = 0; i < s->narms; i++)
		if (s->arm[i].has && (!found || s->arm[i].pm.blk < *blk))
		{
			*blk = s->arm[i].pm.blk;
			found = true;
		}
	if (!found)
		return false;
	memset(mask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
	for (int i = 0; i < s->narms; i++)
		if (s->arm[i].has && s->arm[i].pm.blk == *blk)
		{
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				mask[k] |= s->arm[i].pm.offmask[k];
			arm_next(&s->arm[i]);
		}
	return true;
}

const SiftFilter *
sift_search_range(SiftSearch *s, uint32 lo, uint32 hi)
{
	BlockNumber blk;
	uint64		mask[SIFT_OFFMASK_WORDS];

	sift_filter_reset(s->filter);
	s->want_lo = lo;
	s->want_hi = hi;
	for (int i = 0; i < s->narms; i++)
		arm_start(&s->arm[i]);
	while (union_page(s, &blk, mask))
	{
		sift_filter_add_page(s->filter, blk, mask);
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			s->matches += pg_popcount64(mask[k]);
	}
	s->done_hi = hi;
	return s->filter;
}
