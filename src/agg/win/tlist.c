/*
 * agg/win/tlist.c - the scan tlist of a finished SiftAggV6Window plan, made so that EXPLAIN can print it.
 *
 * PostgreSQL 18 prints a WindowFunc only under the WindowAgg node of its winref, and raises "could not find window clause" anywhere else. The planner
 * must see the WindowFunc nodes in the scan tlist (set_customscan_references matches the plan tlist to it by equal()), so the finished plan swaps each one
 * for an Aggref of the same function, type and collation: it prints as the call (row_number(), sum(v) FILTER (WHERE ...)) and is never evaluated, because
 * the scan tuple is the node's own and the plan tlist reads it by INDEX_VAR. The node finds its functions in its private list, which keeps the WindowFunc nodes.
 */
#include "postgres.h"

#include "catalog/pg_aggregate_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"

#include "agg/win/window.h"

static Aggref *
call_of(const WindowFunc *wf)
{
	Aggref	   *a = makeNode(Aggref);
	int			resno = 1;

	a->aggfnoid = wf->winfnoid;
	a->aggtype = wf->wintype;
	a->aggcollid = wf->wincollid;
	a->inputcollid = wf->inputcollid;
	a->aggstar = wf->winstar;
	a->aggfilter = wf->aggfilter;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = wf->location;
	foreach_ptr(Expr, arg, wf->args)
	{
		a->args = lappend(a->args, makeTargetEntry(arg, resno++, NULL, false));
		a->aggargtypes = lappend_oid(a->aggargtypes, exprType((Node *) arg));
	}
	return a;
}

void
sift_v6_window_finish_tlist(Plan *plan)
{
	CustomScan *cs = (CustomScan *) plan;
	List	   *tlist = NIL;

	if (!IsA(plan, CustomScan) || cs->methods != &sift_agg_v6_window_plan_methods)
		return;
	foreach_node(TargetEntry, te, cs->custom_scan_tlist)
	{
		TargetEntry *out = flatCopyTargetEntry(te);

		if (IsA(te->expr, WindowFunc))
			out->expr = (Expr *) call_of((WindowFunc *) te->expr);
		tlist = lappend(tlist, out);
	}
	cs->custom_scan_tlist = tlist;
}
