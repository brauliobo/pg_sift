/*
 * agg/v6/v6_text.c - a text query (body @@> 'const', or an OR of them on one index) among the quals of an aggregate of a table with a sift_col index of format major 2. The scan of the index reads
 * the rows of the hits only: the hits of the arms on their sift index are read in heap order, without scores, a row matched by several arms once, into chunks of a bridge (agg/hybrid.c, the byte
 * budget of work_mem / 2), and each chunk is a pass of the scan, which skips the granules the chunk misses and every row not in it. The groups of every pass are one. A query that needs the indexed
 * text of its hits, or whose phrase the rows of the write buffer (which have no positions) may leave undecided, is not served: the planner leaves it to the text index's own scan.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/v6/v6_cost_model.h"
#include "agg/hybrid_int.h"
#include "agg/v6/v6.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "core/memtrack.h"
#include "core/sift.h"

typedef struct TextArm
{
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

/* the index of a partitioned table is no storage: only a partition's own index has hits to read */
static IndexOptInfo *
arm_index(RelOptInfo *rel, Node *clause, Const **query)
{
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		Expr	   *q;

		if (ix->relam == sift_catalog()->am && !ix->hypothetical && get_rel_relkind(ix->indexoid) == RELKIND_INDEX && sift_match_clause(clause, ix, &q) && IsA(q, Const) && !((Const *) q)->constisnull &&
			((Const *) q)->consttype == TEXTOID)
		{
			*query = (Const *) q;
			return ix;
		}
	}
	return NULL;
}

IndexOptInfo *
sift_v6_text_match(RelOptInfo *rel, RestrictInfo *ri, List **queries)
{
	List	   *branches = IsA(ri->clause, BoolExpr) && ((BoolExpr *) ri->clause)->boolop == OR_EXPR ? ((BoolExpr *) ri->clause)->args : list_make1(ri->clause);
	IndexOptInfo *text = NULL;

	*queries = NIL;
	foreach_ptr(Node, branch, branches)
	{
		Const	   *query;
		IndexOptInfo *ix = arm_index(rel, branch, &query);

		if (ix == NULL || (text != NULL && ix != text))
			return NULL;
		text = ix;
		*queries = lappend(*queries, query);
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

static SiftPlan *
bind_query(SiftIndexState *ist, Const *query, int flags)
{
	text	   *t = DatumGetTextPP(query->constvalue);
	SiftQuery  *q = sift_state_query(ist, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));

	return sift_plan_bind(ist, &q, 1, flags);
}

bool
sift_v6_text_usable(IndexOptInfo *text, List *queries)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 text query");
	MemoryContext old = MemoryContextSwitchTo(cx);
	Relation	index = index_open(text->indexoid, AccessShareLock);
	SiftIndexState *ist = sift_state_open(index, cx);
	bool		usable = true;

	foreach_ptr(Const, query, queries)
		usable &= !sift_plan_recheck_possible(bind_query(ist, query, SIFT_BIND_CAPPED));
	sift_state_close(ist);
	index_close(index, AccessShareLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return usable;
}

/* The hits cost the dictionary lookups and a little each to read and to put in the bridge: the rows the scan decodes are those of the granules the hits are in */
double
sift_v6_text_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *text, RestrictInfo *ri, List *queries, double *touch)
{
	double		rows = Max(rel->tuples, 1.0),
				granules = Max(1.0, ceil(rows / SIFT_GRANULE_ROWS)),
				sel = clause_selectivity(root, (Node *) ri->clause, rel->relid, JOIN_INNER, NULL);

	*touch = sift_hybrid_granule_hit(sel, rows / granules);
	return sift_dict_cost(text, queries) + rows * sel * cpu_tuple_cost * 0.25;
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
		SiftPlan   *plan = bind_query(t->ist, list_nth_node(Const, spec->text_queries, i), 0);

		SIFT_INVARIANT(!sift_plan_recheck_needed(plan), "sift: the aggregate scan takes no query that needs the text of its hits");
		t->arm[i].it = sift_iter_begin(plan);
		t->arm[i].has = sift_iter_next(t->arm[i].it, &t->arm[i].pm);
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
			t->arm[i].has = sift_iter_next(t->arm[i].it, &t->arm[i].pm);
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
