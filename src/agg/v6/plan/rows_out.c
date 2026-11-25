/*
 * agg/v6/plan/rows_out.c - the columns a row Top-N of SiftAggV6 gives (agg/v6/topn/rows.h): the columns of the table, which the node fetches from the heap rows of the winners, the columns of the
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

#include "agg/v6/plan/int.h"
#include "agg/v6/topn/rows.h"

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

	if (wc == NULL || !whole_set(wc) || !wf->winagg)
		return false;
	a = window_aggref(wf);
	if (!sift_v6_compile_agg_native(p, a))
		return false;
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
	*out = (SiftV6RowOut) {.from = SV6OUT_HEAP};
	return v->varattno > 0 && bms_is_empty(v->varnullingrels);
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

		if (tlist_member((Expr *) n, p->scan_tlist) != NULL)
			continue;
		if (!row_out(p, n, &out))
			return false;
		p->spec.out[p->spec.nout++] = out;
		p->scan_tlist = lappend(p->scan_tlist, makeTargetEntry((Expr *) copyObject(n), p->spec.nout, NULL, false));
	}
	return p->scan_tlist != NIL;
}

/* the dimensions a row Top-N joins, for EXPLAIN */
char *
sift_v6_rows_dims_desc(const V6Plan *p)
{
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_ptr(RelOptInfo, rel, p->dim_rels)
	{
		const RangeTblEntry *rte = rel->reloptkind == RELOPT_BASEREL ? planner_rt_fetch(rel->relid, p->root) : NULL;
		const char *kind = rte == NULL ? "join" : rte->rtekind == RTE_SUBQUERY ? "subquery" : rte->rtekind == RTE_VALUES ? "values" : rte->rtekind == RTE_CTE ? "cte" : NULL;

		appendStringInfo(&buf, "%s%s", buf.len > 0 ? ", " : "", rte != NULL && rte->rtekind == RTE_RELATION ? get_rel_name(rte->relid) : rte != NULL ? rte->eref->aliasname : "");
		if (kind != NULL)
			appendStringInfo(&buf, " (%s)", kind);
	}
	return buf.data;
}
