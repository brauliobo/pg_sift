/*
 * agg/v6/v6_text.c - a text query (body @@> 'const') among the quals of an aggregate of a table with a sift_col index of format major 2. The scan of the index reads the rows of
 * the hits only: the hits of the query on its sift index are read in heap order, without scores, into chunks of a bridge (agg/hybrid.c, the byte budget of work_mem / 2), and each
 * chunk is a pass of the scan, which skips the granules the chunk misses and every row not in it. The groups of every pass are one. A query with a phrase is not served: the rows of the
 * write buffer have no positions, so the text of its hits may be needed at any execution, and the planner leaves it to the text index's own scan.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/hybrid_int.h"
#include "agg/v6/v6.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "col/f2/f2.h"
#include "core/memtrack.h"
#include "core/sift.h"

struct SiftV6Text
{
	MemoryContext cx;
	Relation	index;
	SiftIndexState *ist;
	SiftPlan   *plan;
	SiftHybridRun *run;
	SiftMatchKeys keys;
};

static const SiftHybridStore v6_text_store = {.host = SIFT_HYB_HOST_TABLE};

IndexOptInfo *
sift_v6_text_match(RelOptInfo *rel, RestrictInfo *ri, Const **query)
{
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		Expr	   *q;

		if (ix->relam == sift_catalog()->am && !ix->hypothetical && sift_match_clause((Node *) ri->clause, ix, &q) && IsA(q, Const) &&
			!((Const *) q)->constisnull && ((Const *) q)->consttype == TEXTOID)
		{
			*query = (Const *) q;
			return ix;
		}
	}
	return NULL;
}

static SiftPlan *
bind_query(SiftIndexState *ist, Const *query, int flags)
{
	text	   *t = DatumGetTextPP(query->constvalue);
	SiftQuery  *q = sift_state_query(ist, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));

	return sift_plan_bind(ist, &q, 1, flags);
}

bool
sift_v6_text_usable(IndexOptInfo *text, Const *query)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 text query");
	MemoryContext old = MemoryContextSwitchTo(cx);
	Relation	index = index_open(text->indexoid, AccessShareLock);
	SiftIndexState *ist = sift_state_open(index, cx);
	bool		usable = !sift_plan_recheck_possible(bind_query(ist, query, SIFT_BIND_CAPPED));

	sift_state_close(ist);
	index_close(index, AccessShareLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return usable;
}

/* The hits cost the dictionary lookups and a little each to read and to put in the bridge: the rows the scan decodes are those of the granules the hits are in */
double
sift_v6_text_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *text, RestrictInfo *ri, Const *query, double *touch)
{
	double		rows = Max(rel->tuples, 1.0),
				granules = Max(1.0, ceil(rows / SIFT_GRANULE_ROWS)),
				sel = clause_selectivity(root, (Node *) ri->clause, rel->relid, JOIN_INNER, NULL);

	*touch = sift_hybrid_granule_hit(sel, rows / granules);
	return sift_dict_cost(text, list_make1(query)) + rows * sel * cpu_tuple_cost * 0.25;
}

SiftV6Text *
sift_v6_text_open(const SiftV6Spec *spec, MemoryContext parent)
{
	MemoryContext cx = sift_mem_allocset_create(parent, "sift agg v6 text");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV6Text *t = palloc0(sizeof(SiftV6Text));

	t->cx = cx;
	t->index = index_open(spec->text_indexoid, AccessShareLock);
	t->ist = sift_state_open(t->index, cx);
	t->plan = bind_query(t->ist, spec->text_query, 0);
	SIFT_INVARIANT(!sift_plan_recheck_needed(t->plan), "sift: the aggregate scan takes no query that needs the text of its hits");
	t->keys.it = sift_iter_begin(t->plan);
	t->run = sift_hybrid_begin(cx, SIFT_HYB_BM25_TO_COL, &v6_text_store, (Size) work_mem * 1024 / 2, NULL, NULL);
	MemoryContextSwitchTo(old);
	return t;
}

bool
sift_v6_text_chunk(void *arg, const SiftTidBridge **bridge)
{
	SiftV6Text *t = arg;

	if (!sift_hybrid_b_chunk(t->run, sift_match_keys_next, &t->keys, false))
		return false;
	*bridge = t->run->br;
	return true;
}

void
sift_v6_text_close(SiftV6Text *t)
{
	sift_iter_end(t->keys.it);
	sift_hybrid_end(t->run);
	sift_state_close(t->ist);
	index_close(t->index, AccessShareLock);
	MemoryContextDelete(t->cx);
}
