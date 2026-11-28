/*
 * agg/plan/rows_out.c - the columns a row Top-N of SiftAggV6 gives (agg/topn/rows.h): the columns of the table, which the node fetches from the heap rows of the winners or the scan decodes, the columns of the
 * dimensions of a join, which the heap of the winners carries as keys the pipe fills, and the window aggregates over the whole set (count(*) OVER (), sum(x) OVER ()), which the scan folds from every
 * row it keeps into one group that every winner shows.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"

#include "agg/plan/int.h"
#include "agg/topn/rows.h"

/* a window that frames the whole set: no partition, no order, and a frame from the first row to the last (or to the peers of the current row, which are all of them without an order) */
static bool
whole_set(const WindowClause *wc)
{
	const int	f = wc->frameOptions;
	const bool	to_end = (f & FRAMEOPTION_END_UNBOUNDED_FOLLOWING) != 0 || ((f & FRAMEOPTION_RANGE) != 0 && (f & FRAMEOPTION_END_CURRENT_ROW) != 0);

	return wc->partitionClause == NIL && wc->orderClause == NIL && wc->startOffset == NULL && wc->endOffset == NULL && (f & FRAMEOPTION_EXCLUSION) == 0 &&
		(f & FRAMEOPTION_START_UNBOUNDED_PRECEDING) != 0 && to_end;
}

static const WindowClause *
window_of(const Query *q, const WindowFunc *wf)
{
	foreach_node(WindowClause, wc, q->windowClause)
		if (wc->winref == wf->winref)
			return wc;
	return NULL;
}

/* the aggregate of the same name a window function over the whole set is */
static Aggref *
window_aggref(const WindowFunc *wf)
{
	Aggref	   *a = makeNode(Aggref);
	int			n = 0;

	a->aggfnoid = wf->winfnoid;
	a->aggtype = wf->wintype;
	a->aggcollid = wf->wincollid;
	a->inputcollid = wf->inputcollid;
	a->aggstar = wf->winstar;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggfilter = wf->aggfilter;
	a->location = wf->location;
	foreach_ptr(Expr, arg, wf->args)
	{
		a->args = lappend(a->args, makeTargetEntry(arg, ++n, NULL, false));
		a->aggargtypes = lappend_oid(a->aggargtypes, exprType((Node *) arg));
	}
	return a;
}

static void
add_agg_desc(V6Plan *p, const Aggref *a)
{
	const Oid	relid = planner_rt_fetch(p->rel->relid, p->root)->relid;
	Aggref	   *named = (Aggref *) copyObject((const Node *) a);
	char	   *desc;

	ChangeVarNodes((Node *) named, p->rel->relid, 1, 0);
	desc = deparse_expression((Node *) named, deparse_context_for(get_rel_name(relid), relid), true, false);
	p->spec.agg_desc = p->spec.agg_desc != NULL ? psprintf("%s, %s", p->spec.agg_desc, desc) : desc;
}

static bool
total_out(V6Plan *p, const WindowFunc *wf, SiftV6RowOut *out)
{
	const WindowClause *wc = window_of(p->root->parse, wf);
	Aggref	   *a;

	if (wc == NULL || !whole_set(wc))
	{
		sift_agg_arm_refused_by("topn", AGG_REFUSE_TOPN_SHAPE, "a window has PARTITION BY, ORDER BY or a frame that is not all the rows");
		return false;
	}
	if (!wf->winagg)
	{
		sift_agg_arm_refused_by("topn", AGG_REFUSE_TOPN_SHAPE,
								psprintf("%s() OVER is not an aggregate: only aggregates over the whole set are computed with the top-N", get_func_name(wf->winfnoid)));
		return false;
	}
	a = window_aggref(wf);
	if (!sift_v6_compile_agg_native(p, a))
	{
		sift_agg_arm_refused_by("topn", AGG_REFUSE_AGGREGATE, psprintf("%s OVER () is not an aggregate the scan computes over its arguments and filter", get_func_name(wf->winfnoid)));
		return false;
	}
	add_agg_desc(p, a);
	*out = (SiftV6RowOut) {.from = SV6OUT_TOTAL,.at = p->spec.naggs - 1};
	return true;
}

/* a column of a dimension is a key the pipe fills for every pair, which the heap of the winners carries (the one of a sort key too) */
static bool
dim_out(V6Plan *p, const Var *v, SiftV6RowOut *out)
{
	SiftV6Col	col;
	int			k;

	if (!sift_v6_star_column(p, (const Expr *) v, &col))
		return false;
	for (k = 0; k < p->spec.nkeys; k++)
		if (p->spec.key[k].src == SV6SRC_DIM && p->spec.key[k].dim == col.dim && p->spec.key[k].slot == col.slot)
			break;
	if (k == p->spec.nkeys)
	{
		if (k == SIFT_V6_MAX_KEYS)
			return false;
		p->spec.key[p->spec.nkeys++] = col;
	}
	*out = (SiftV6RowOut) {.from = SV6OUT_KEY,.at = k};
	return true;
}

/* a column of the table the scan decodes as a key the heap of winners carries (the one of a sort key too) */
static bool
store_out(V6Plan *p, const Var *v, SiftV6RowOut *out)
{
	SiftV6Col	col;
	int			k = 0;

	if (!sift_v6_compile_sort_key(p, (Expr *) v, &col))
		return false;
	while (k < p->spec.nkeys && !sift_v6_col_same(&p->spec.key[k], &col))
		k++;
	if (k == SIFT_V6_MAX_KEYS)
		return false;
	if (k == p->spec.nkeys)
		p->spec.key[p->spec.nkeys++] = col;
	*out = (SiftV6RowOut) {.from = SV6OUT_KEY,.at = k};
	return true;
}

static bool
row_out(V6Plan *p, const Node *n, SiftV6RowOut *out)
{
	const Var  *v = (const Var *) n;

	if (IsA(n, WindowFunc))
		return total_out(p, (const WindowFunc *) n, out);
	if (!IsA(n, Var) || v->varlevelsup != 0)
		return false;
	if (v->varno != p->rel->relid)
		return dim_out(p, v, out);
	if (v->varattno <= 0 || !bms_is_empty(v->varnullingrels))
		return false;
	if (p->rows_from_store)
		return store_out(p, v, out);
	*out = (SiftV6RowOut) {.from = SV6OUT_HEAP};
	return true;
}

static Node *
window_aggs_mutator(Node *n, void *context)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, WindowFunc))
		return (Node *) window_aggref((const WindowFunc *) n);
	return expression_tree_mutator(n, window_aggs_mutator, context);
}

/* the window functions of a row Top-N as the aggregates the scan gives: no WindowAgg sits above the node, and a WindowFunc has no WindowClause to be printed from */
Node *
sift_v6_rows_window_aggs(Node *expr)
{
	return window_aggs_mutator(expr, NULL);
}

bool
sift_v6_rows_outputs(V6Plan *p)
{
	List	   *vars = pull_var_clause((Node *) p->out->reltarget->exprs, PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS);

	p->scan_tlist = NIL;
	p->spec.out = palloc(Max(list_length(vars), 1) * sizeof(SiftV6RowOut));
	foreach_ptr(Node, n, vars)
	{
		SiftV6RowOut out;
		Expr	   *entry = (Expr *) sift_v6_rows_window_aggs(n);

		if (tlist_member(entry, p->scan_tlist) != NULL)
			continue;
		if (!row_out(p, n, &out))
			return false;
		p->spec.out[p->spec.nout++] = out;
		p->scan_tlist = lappend(p->scan_tlist, makeTargetEntry(entry, p->spec.nout, NULL, false));
	}
	return p->scan_tlist != NIL;
}
