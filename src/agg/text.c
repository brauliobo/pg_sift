/*
 * agg/text.c - a search (body @@> 'const'; an OR of them on one index, and ANDs of those) among the quals of an aggregate of a table with a sift_col index of format major 2. The scan of the index
 * reads the rows of the hits only: the hits of the arms on their sift index (an arm binds the queries it ANDs into one plan) are read in heap order, without scores, a row matched by several arms
 * once, into chunks of a bridge (agg/hybrid/hybrid.c, the byte budget of work_mem / 2), and each chunk is a pass of the scan, which skips the granules the chunk misses and every row not in it. The
 * groups of every pass are one. A query that needs the indexed text of its hits, or whose phrase the rows of the write buffer (which have no positions) may leave undecided, is not served: the
 * planner leaves it to the text index's own scan.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/cost/model.h"
#include "agg/hybrid/int.h"
#include "agg/engine.h"
#include "agg/settings.h"
#include "bm25/planner/planner.h"
#include "bm25/planner/int.h"
#include "core/memtrack.h"
#include "core/sift.h"

typedef struct TextArm
{
	SiftPlan   *plan;
	SiftMatchIter *it;
	SiftPageMatch pm;			/* the arm's next page, when has */
	bool		has;
} TextArm;

struct SiftV6Text
{
	MemoryContext cx;
	Relation	index;
	SiftIndexState *ist;
	SiftHybridRun *run;
	int			narms;
	TextArm    *arm;
	SiftPageMatch page;			/* the page of the union being handed out */
	int			word;
};

static const SiftHybridStore v6_text_store = {.host = SIFT_HYB_HOST_TABLE};

bool
sift_v6_text_index(const IndexOptInfo *ix)
{
	return ix->relam == sift_catalog()->am && !ix->hypothetical && (ix->indpred == NIL || ix->predOK);
}

/* a query whose text the plan has: a text constant */
static bool
query_given(const Expr *q)
{
	return IsA(q, Const) && !((const Const *) q)->constisnull && ((const Const *) q)->consttype == TEXTOID;
}

/* the index of a partitioned table is no storage: only a partition's own index has hits to read */
static IndexOptInfo *
arm_index(RelOptInfo *rel, Node *clause, Expr **query)
{
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if (sift_v6_text_index(ix) && get_rel_relkind(ix->indexoid) == RELKIND_INDEX && sift_match_clause(clause, ix, query) && query_given(*query))
			return ix;
	return NULL;
}

IndexOptInfo *
sift_v6_text_match(RelOptInfo *rel, RestrictInfo *ri, List **branches)
{
	List	   *clauses = IsA(ri->clause, BoolExpr) && ((BoolExpr *) ri->clause)->boolop == OR_EXPR ? ((BoolExpr *) ri->clause)->args : list_make1(ri->clause);
	IndexOptInfo *text = NULL;

	*branches = NIL;
	foreach_ptr(Node, branch, clauses)
	{
		Expr	   *query;
		IndexOptInfo *ix = arm_index(rel, branch, &query);

		if (ix == NULL || (text != NULL && ix != text))
			return NULL;
		text = ix;
		*branches = lappend(*branches, query);
	}
	return text;
}

/* The clause is a match of a text index of the relation (a text query the index answers, whatever its query): no scan key or batch qual of the aggregate decides it */
bool
sift_v6_text_clause(RelOptInfo *rel, RestrictInfo *ri)
{
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		Expr	   *q;

		if (ix->relam == sift_catalog()->am && !ix->hypothetical && sift_match_clause((Node *) ri->clause, ix, &q))
			return true;
	}
	return false;
}

/* The plan of an arm: the AND of its queries */
static SiftPlan *
arm_plan(SiftIndexState *ist, List *queries, int flags)
{
	SiftQuery **q = palloc(list_length(queries) * sizeof(SiftQuery *));
	int			n = 0;

	foreach_node(Const, c, queries)
	{
		const text *t = DatumGetTextPP(c->constvalue);

		q[n++] = sift_state_query(ist, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	}
	return sift_plan_bind(ist, q, n, flags);
}

bool
sift_v6_text_usable(IndexOptInfo *text, List *arms)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 text query");
	MemoryContext old = MemoryContextSwitchTo(cx);
	Relation	index = index_open(text->indexoid, AccessShareLock);
	SiftIndexState *ist = sift_state_open(index, cx);
	bool		usable = true;

	foreach_node(List, arm, arms)
		usable &= !sift_plan_recheck_possible(arm_plan(ist, arm, SIFT_BIND_CAPPED));
	sift_state_close(ist);
	index_close(index, AccessShareLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return usable;
}

/* the operators of a posting walked and put in the bridge, at the scan's own price of a millisecond (0.41 ms more for 5,000 postings of a word than for one) */
#define SEARCH_BRIDGE_OPS	6.0

/* The hits cost the dictionary lookups and each posting its walk into the bridge: the rows the scan decodes are those of the granules the hits are in */
double
sift_v6_text_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *text, List *quals, List *arms, double *touch)
{
	double		rows = Max(rel->tuples, 1.0),
				granules = Max(1.0, ceil(rows / SIFT_GRANULE_ROWS)),
				sel = clauselist_selectivity(root, quals, rel->relid, JOIN_INNER, NULL);
	List	   *queries = NIL;

	foreach_node(List, arm, arms)
		queries = list_concat(queries, arm);
	*touch = sift_hybrid_granule_hit(sel, rows / granules);
	return sift_dict_cost(text, queries) + rows * sel * SEARCH_BRIDGE_OPS * cpu_operator_cost;
}

/* the instructions to walk a posting of the word, and to read the row a key gives and check its text */
#define SEARCH_POSTING_OPS	320.0
#define SEARCH_KEY_ROW_OPS	530.0

/* a btree-like index of the relation reads the rows of the condition by its first column: a comparison of the column with a value of no column of the relation, or IS NULL */
static bool
key_clause_of_index(PlannerInfo *root, const RelOptInfo *rel, const IndexOptInfo *ix, const Expr *clause)
{
	const Node *col = NULL,
			   *other = NULL;

	if (IsA(clause, NullTest))
		col = (const Node *) ((const NullTest *) clause)->arg;
	else if (IsA(clause, OpExpr) && list_length(((const OpExpr *) clause)->args) == 2 && op_in_opfamily(((const OpExpr *) clause)->opno, ix->opfamily[0]))
	{
		col = linitial(((const OpExpr *) clause)->args);
		other = lsecond(((const OpExpr *) clause)->args);
		if (!(IsA(col, Var) && ((const Var *) col)->varno == rel->relid))
		{
			col = lsecond(((const OpExpr *) clause)->args);
			other = linitial(((const OpExpr *) clause)->args);
		}
	}
	return col != NULL && IsA(col, Var) && ((const Var *) col)->varno == rel->relid && ((const Var *) col)->varattno == ix->indexkeys[0] &&
		(other == NULL || !bms_is_member(rel->relid, pull_varnos(root, (Node *) other)));
}

static bool
reads_by_key(PlannerInfo *root, const RelOptInfo *rel, const List *own)
{
	if (!enable_indexscan && !enable_bitmapscan)
		return false;
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		if (ix->hypothetical || ix->indpred != NIL || ix->indexkeys[0] == 0 || ix->relam == sift_catalog()->am || ix->relam == sift_catalog()->col_am)
			continue;
		foreach_node(RestrictInfo, ri, own)
			if (key_clause_of_index(root, rel, ix, ri->clause))
				return true;
	}
	return false;
}

bool
sift_v6_search_key_first(PlannerInfo *root, RelOptInfo *rel, double postings)
{
	List	   *own = NIL;

	if (sift_agg_force)
		return false;
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (!sift_v6_text_clause(rel, ri))
			own = lappend(own, ri);
	return own != NIL && reads_by_key(root, rel, own) &&
		Max(rel->tuples, 1.0) * clauselist_selectivity(root, own, rel->relid, JOIN_INNER, NULL) * SEARCH_KEY_ROW_OPS < postings * SEARCH_POSTING_OPS;
}

/* The arm's next page */
static void
arm_next(SiftV6Text *t, TextArm *a)
{
	a->has = sift_iter_next(a->it, &a->pm);
}

SiftV6Text *
sift_v6_text_open(const SiftV6Spec *spec, MemoryContext parent)
{
	MemoryContext cx = sift_mem_allocset_create(parent, "sift agg v6 text");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV6Text *t = palloc0(sizeof(SiftV6Text));

	t->cx = cx;
	t->index = index_open(spec->text_indexoid, AccessShareLock);
	pgstat_count_index_scan(t->index);
	t->ist = sift_state_open(t->index, cx);
	t->narms = list_length(spec->text_queries);
	t->arm = palloc0(t->narms * sizeof(TextArm));
	for (int i = 0; i < t->narms; i++)
	{
		TextArm    *a = &t->arm[i];

		a->plan = arm_plan(t->ist, list_nth_node(List, spec->text_queries, i), 0);
		SIFT_INVARIANT(!sift_plan_recheck_needed(a->plan), "sift: the aggregate scan takes no query that needs the text of its hits");
		a->it = sift_iter_begin(a->plan);
		arm_next(t, a);
	}
	t->word = SIFT_OFFMASK_WORDS;
	t->run = sift_hybrid_begin(cx, SIFT_HYB_BM25_TO_COL, &v6_text_store, (Size) work_mem * 1024 / 2, NULL, NULL);
	MemoryContextSwitchTo(old);
	return t;
}

/* The next page of the union of the arms in heap order: the offsets that any arm having the page matches */
static bool
union_page(SiftV6Text *t)
{
	bool		found = false;

	for (int i = 0; i < t->narms; i++)
		if (t->arm[i].has && (!found || t->arm[i].pm.blk < t->page.blk))
		{
			t->page.blk = t->arm[i].pm.blk;
			found = true;
		}
	if (!found)
		return false;
	memset(t->page.offmask, 0, sizeof(t->page.offmask));
	for (int i = 0; i < t->narms; i++)
		if (t->arm[i].has && t->arm[i].pm.blk == t->page.blk)
		{
			for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
				t->page.offmask[w] |= t->arm[i].pm.offmask[w];
			arm_next(t, &t->arm[i]);
		}
	t->word = 0;
	return true;
}

static bool
union_next(void *arg, SiftKey *key, float4 *score)
{
	SiftV6Text *t = arg;

	*score = 0;
	while (!sift_match_page_next(&t->page, &t->word, key))
		if (!union_page(t))
			return false;
	return true;
}

bool
sift_v6_text_chunk(void *arg, const SiftTidBridge **bridge)
{
	SiftV6Text *t = arg;

	if (!sift_hybrid_b_chunk(t->run, union_next, t, false))
		return false;
	*bridge = t->run->br;
	return true;
}

void
sift_v6_text_close(SiftV6Text *t)
{
	for (int i = 0; i < t->narms; i++)
		sift_iter_end(t->arm[i].it);
	sift_hybrid_end(t->run);
	sift_state_close(t->ist);
	index_close(t->index, AccessShareLock);
	MemoryContextDelete(t->cx);
}
