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
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "agg/colsrc.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "bm25/planner.h"
#include "agg/search.h"

/*
 * What the match set costs, in operator costs: a posting decoded and combined, a matching row decoded and aggregated by the scan, a page of matches
 * merged and copied, a term looked up. A posting is 48 instructions measured (a top-N over 20 words opens 470,000 of them in 22.7 M), a matching row
 * of a counted scan 196 more (244 a match in all: a count over a word in 62,500 of 250,000 rows is 15.25 M instructions, 6.1 M of 25,000 at 100,000
 * rows); an instruction is 0.19 operators
 */
#define SEARCH_POSTING		9.0
#define SEARCH_MATCH		37.0
#define SEARCH_PAGE			12.0
#define SEARCH_TERM			400.0
#define SEARCH_BIND			20000.0	/* binding the queries: the directory, the analyzer, the dictionaries (about 60 us) */
/* A value decoded by position costs this many dense ones (docs/COL-SEARCH.md, "Measured"); beyond a fraction 1 / this of a unit's rows the unit is read whole */
#define SEARCH_SPARSE		8.0
#define SEARCH_KEY_ROW		530.0	/* a row read by an index and checked for the text on the heap: 3,600 instructions measured on 500 keys, at the 6.8 instructions an operator that the postings of 25,000 and 62,500 matches measured */

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
	return sift_search_open_cost(sp, nrows) + pages * SEARCH_PAGE + docs * SEARCH_MATCH;
}

/*
 * Do the other conditions of the WHERE keep so few rows that an index of the table reads them and checks the text on the heap for less than the scan walks the
 * postings of the search? The match set is read whole, the rows of the keys by position: a key of 500 rows costs PostgreSQL's plan 1.8 M instructions at any size of
 * the table, the postings of a word in a quarter of the rows cost the scan 3.0, 8.0 and 19.7 M at 30,000, 100,000 and 250,000 rows. Both in the price's own operators;
 * PostgreSQL prices its plan by pages it assumes cold, which is why the two prices alone keep choosing the scan.
 */
bool
sift_search_key_first(PlannerInfo *root, RelOptInfo *rel, const SiftSearchPlan *sp, List *rest, double *scan_ops, double *key_ops)
{
	double		units = Max(rel->tuples / 2000.0, 1.0),
				rows_read,
				units_read;
	bool		indexed = false;
	List	   *own = NIL;

	/* the conditions on this table alone: a join clause keeps rows of the join, which the key of an index does not read */
	foreach_ptr(Node, q, rest)
		if (bms_equal(pull_varnos(root, q), bms_make_singleton(rel->relid)))
			own = lappend(own, q);
	if (own == NIL)
		return false;
	foreach_ptr(Path, p, rel->pathlist)
		if ((IsA(p, IndexPath) && ((IndexPath *) p)->indexinfo->relam != sift_catalog()->am && ((IndexPath *) p)->indexclauses != NIL) || IsA(p, BitmapHeapPath))
			indexed = true;
	if (!indexed)
		return false;
	*scan_ops = sift_search_cost(sp, Max(rel->tuples, 1.0), units, &rows_read, &units_read);
	*key_ops = Max(rel->tuples, 1.0) * clauselist_selectivity(root, own, rel->relid, JOIN_INNER, NULL) * SEARCH_KEY_ROW;
	return *scan_ops > *key_ops;
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
	bool		chains_done;	/* the table has HOT chains: the filter of every unit was made at the first call */
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

/* A page of the match set of a table with HOT chains, with the members of the chains of the matches added (see search_with_chains) */
typedef struct HotPage
{
	BlockNumber blk;
	uint64		mask[SIFT_OFFMASK_WORDS];
} HotPage;

static void
hot_member(void *arg, SiftKey key)
{
	BlockNumber blk = key >> 16;
	uint32		off = (key & 0xFFFF) - 1;
	bool		found;
	HotPage    *p = hash_search((HTAB *) arg, &blk, HASH_ENTER, &found);

	if (!found)
		memset(p->mask, 0, sizeof(p->mask));
	p->mask[off / 64] |= UINT64CONST(1) << (off % 64);
}

static int
hot_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(((const HotPage *) a)->blk, ((const HotPage *) b)->blk);
}

/*
 * The match set of every unit, with the members of the HOT chains of the matches: the index has an entry for the first version of a chain
 * and the scans of the table see the version the snapshot sees, wherever it is, and the members hold the text the root does. Made on the
 * first call; the rows the table does not keep for the snapshot leave through its own visibility.
 */
static const SiftFilter *
search_with_chains(SiftSearch *s)
{
	HASHCTL		ctl = {.keysize = sizeof(BlockNumber),.entrysize = sizeof(HotPage),.hcxt = CurrentMemoryContext};
	HTAB	   *members = hash_create("sift search members of chains", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	HASH_SEQ_STATUS seq;
	HotPage    *p,
			   *roots = palloc(256 * sizeof(HotPage)),
			   *later;
	int			nroots = 0,
				maxroots = 256,
				nlater = 0,
				r = 0;
	BlockNumber blk;
	uint64		mask[SIFT_OFFMASK_WORDS];

	sift_filter_reset(s->filter);
	s->want_lo = 0;
	s->want_hi = PG_UINT32_MAX;
	for (int i = 0; i < s->narms; i++)
		arm_start(&s->arm[i]);
	/* the pages come in ascending order; the members, which lie in later pages than their roots, are collected by page and merged in */
	while (union_page(s, &blk, mask))
	{
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			s->matches += pg_popcount64(mask[k]);
		if (nroots == maxroots)
			roots = repalloc(roots, (maxroots *= 2) * sizeof(HotPage));
		roots[nroots].blk = blk;
		memcpy(roots[nroots++].mask, mask, sizeof(mask));
		sift_vis_chain_members(s->heap, blk, mask, hot_member, members);
	}
	later = palloc(hash_get_num_entries(members) * sizeof(HotPage) + 1);
	hash_seq_init(&seq, members);
	while ((p = hash_seq_search(&seq)) != NULL)
		later[nlater++] = *p;
	qsort(later, nlater, sizeof(HotPage), hot_cmp);
	for (int i = 0; i < nlater || r < nroots;)
	{
		HotPage		page;

		if (r < nroots && (i >= nlater || roots[r].blk <= later[i].blk))
			page = roots[r++];
		else
			page = later[i];
		if (i < nlater && later[i].blk == page.blk)
		{
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				page.mask[k] |= later[i].mask[k];
			i++;
		}
		sift_filter_add_page(s->filter, page.blk, page.mask);
	}
	pfree(roots);
	pfree(later);
	hash_destroy(members);
	s->chains_done = true;
	return s->filter;
}

const SiftFilter *
sift_search_range(SiftSearch *s, uint32 lo, uint32 hi)
{
	BlockNumber blk;
	uint64		mask[SIFT_OFFMASK_WORDS];

	if (s->chains_done)
		return s->filter;
	if (sift_vis_has_chains(s->heap))
		return search_with_chains(s);
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
