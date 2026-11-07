/*
 * agg/latest.c - the first rows of each group, asked with a window function.
 *
 * `SELECT .. FROM (SELECT .., row_number() OVER (PARTITION BY k ORDER BY o) rn FROM t) s WHERE rn = 1` keeps the first row of o for each k. That
 * is `SELECT DISTINCT ON (k) .. FROM t ORDER BY k, o`, which the scan runs in one pass over the rows where the window sorts all of them. The
 * rewrite is made where the query is that one: the sub-select has the one window function, the outer WHERE keeps its first row (`= 1`, `<= 1`,
 * `< 2`), so every row that is left has rn = 1 and the column reads 1 anywhere the query uses it. Rows of a tie in o are in no order in the
 * window and in DISTINCT ON: the row kept is one of the tie in both. The planner hook plans the query both ways, as for the lateral
 * sub-selects of flat/, and keeps the rewritten one only if it runs in the scan.
 *
 * The first N rows (`rn <= N`, N > 1) are the group's array of each column, ordered by o, cut to N elements and unnested with their ordinal:
 * `(SELECT k, (array_agg(c ORDER BY o))[1:N] a FROM t GROUP BY k) g, LATERAL unnest(g.a) WITH ORDINALITY u(c, rn)`. The scan runs the
 * aggregates where the window sorts all the rows, and a group keeps only the N rows its slice reads (SiftAggGen.topn, agg/gen_run.c): the memory is
 * N rows for each group, and the scan refuses a query whose groups would not fit the hash aggregate's memory (first_memory). Where the plan
 * is not the scan's, PostgreSQL sorts the rows of every group for its arrays: the plan is chosen by cost against the window's.
 *
 * rank() <= N and dense_rank() <= N (up to 1,000, N = 1 included) keep the rows that tie: the arrays are not cut (PostgreSQL would lose the ties), the aggregates
 * carry a mark in their location (SIFT_TOPN_MARK) that only the scan reads, and the scan keeps the rows of rank <= N, or of the N best values, of each group. The
 * window function stays in the query, over the unnested rows (rank and dense_rank of the rows that are left are the rows' own, since every row that sorts before
 * one of them is left too), and so does the condition on it. The sort and partition expressions are arrays of their own when the sub-select does not read them.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "nodes/primnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"
#include "agg/refuse.h"

/*
 * The rows `rn = 1`, `rn <= n` or `rn < n + 1` (or the same with the operands swapped) keep of the range table entry rti's column attno: n, or 0
 * for another condition.
 */
static int64
row_bound(Node *qual, Index rti, AttrNumber *attno)
{
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *c;
	const char *name;
	int64		n;
	bool		swapped;

	if (!IsA(qual, OpExpr) || list_length((op = (OpExpr *) qual)->args) != 2 || get_func_namespace(op->opfuncid) != PG_CATALOG_NAMESPACE)
		return 0;
	l = linitial(op->args);
	r = lsecond(op->args);
	swapped = IsA(l, Const);
	if (swapped)
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (!IsA(l, Var) || !IsA(r, Const) || ((Var *) l)->varno != rti || ((Var *) l)->varlevelsup != 0 || ((Var *) l)->vartype != INT8OID)
		return 0;
	c = (Const *) r;
	if (c->constisnull)
		return 0;
	switch (c->consttype)
	{
		case INT2OID:
			n = DatumGetInt16(c->constvalue);
			break;
		case INT4OID:
			n = DatumGetInt32(c->constvalue);
			break;
		case INT8OID:
			n = DatumGetInt64(c->constvalue);
			break;
		default:
			return 0;
	}
	name = get_opname(op->opno);
	if (name == NULL)
		return 0;
	*attno = ((Var *) l)->varattno;
	if (strcmp(name, "=") == 0)
		return n == 1 ? 1 : 0;
	if (strcmp(name, swapped ? ">=" : "<=") == 0)
		return Max(n, 0);
	if (strcmp(name, swapped ? ">" : "<") == 0)
		return Max(n - 1, 0);
	return 0;
}

SIFT_WALKER(count_window_funcs, int *, count,
	if (IsA(n, WindowFunc))
		(*count)++;)

static int
expr_index(List *list, Node *e)
{
	foreach_ptr(Node, x, list)
		if (equal(x, e))
			return foreach_current_index(x);
	return -1;
}

/* the aggregate array_agg(e ORDER BY the window's order): the order expressions are arguments of its own, numbered from 1 */
static Aggref *
array_of(Expr *e, List *order, List *tlist, int mark)
{
	Aggref	   *a = makeNode(Aggref);
	Oid			t = exprType((Node *) e);
	int			ref = 0;

	a->aggfnoid = F_ARRAY_AGG_ANYNONARRAY;
	a->aggtype = get_array_type(t);
	a->inputcollid = exprCollation((Node *) e);
	a->aggargtypes = list_make1_oid(t);
	a->args = list_make1(makeTargetEntry(copyObject(e), 1, NULL, false));
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = mark;
	foreach_node(SortGroupClause, o, order)
	{
		SortGroupClause *c = copyObject(o);
		TargetEntry *te = copyObject(get_sortgroupclause_tle(o, tlist));

		te->resno = list_length(a->args) + 1;
		te->resjunk = true;
		te->ressortgroupref = c->tleSortGroupRef = ++ref;
		a->args = lappend(a->args, te);
		a->aggorder = lappend(a->aggorder, c);
	}
	return a;
}

/*
 * The sub-select with its window as the groups' arrays cut at n elements: the query that reads the sub-select reads the same columns in the same
 * positions, and rn is the ordinal of the element. NULL (and the reason) for a column the arrays cannot hold.
 */
static Query *
topn_array(Query *sub, WindowClause *w, TargetEntry *rn, int64 n, int kind, const char **why)
{
	Query	   *g = copyObject(sub),
			   *top = makeNode(Query);
	RangeTblEntry *gr = makeNode(RangeTblEntry),
			   *ur = makeNode(RangeTblEntry);
	RangeTblFunction *rf;
	List	   *gnames = NIL,
			   *unames = NIL,
			   *keys = NIL,		/* the expressions of the partition: columns 1.. of the grouped query */
			   *cols = NIL,		/* the expressions that have an array, after the keys */
			   *funcs = NIL,
			   *tlist = NIL;
	int			nkeys;
	bool		junk_last = false;

	g->targetList = NIL;
	foreach_node(SortGroupClause, k, w->partitionClause)
	{
		TargetEntry *te = copyObject(get_sortgroupclause_tle(k, sub->targetList));

		keys = lappend(keys, te->expr);
		te->resjunk = false;
		te->resno = list_length(keys);
		te->resname = psprintf("k%d", te->resno);
		gnames = lappend(gnames, makeString(te->resname));
		g->targetList = lappend(g->targetList, te);
	}
	nkeys = list_length(keys);
	foreach_node(TargetEntry, te, sub->targetList)
	{
		if (te->resjunk)
			junk_last = true;
		else if (junk_last)
		{
			*why = "the sub-select's columns are not in order";
			return NULL;
		}
		if (te == rn || (te->resjunk && kind == SIFT_TOPN_ROWS))
			continue;
		if (expr_index(keys, (Node *) te->expr) < 0 && expr_index(cols, (Node *) te->expr) < 0)
		{
			Oid			t = exprType((Node *) te->expr);

			if (!OidIsValid(get_array_type(t)) || type_is_array(t) || type_is_rowtype(t))
			{
				*why = "a column has a type with no array of its own, or is an array or a row";
				return NULL;
			}
			cols = lappend(cols, te->expr);
		}
	}
	if (cols == NIL)
	{
		*why = "the sub-select reads nothing but the partition and the row number";
		return NULL;
	}
	foreach_ptr(Expr, e, cols)
	{
		Oid			t = exprType((Node *) e);
		Oid			at = get_array_type(t);
		SubscriptingRef *cut = makeNode(SubscriptingRef);
		TargetEntry *te;

		/* each array cut at n inside the group: the aggregate is read only for its first n elements, which the scan keeps and nothing else */
		cut->refcontainertype = cut->refrestype = at;
		cut->refelemtype = t;
		cut->reftypmod = -1;
		cut->refupperindexpr = list_make1(makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum((int32) Min(n, PG_INT32_MAX)), false, true));
		cut->reflowerindexpr = list_make1(makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(1), false, true));
		cut->refexpr = (Expr *) array_of(e, w->orderClause, sub->targetList, kind == SIFT_TOPN_ROWS || n > SIFT_AGG_TOPN_MAX ? -1 : SIFT_TOPN_MARK(kind, n));
		te = makeTargetEntry(kind == SIFT_TOPN_ROWS ? (Expr *) cut : cut->refexpr, nkeys + foreach_current_index(e) + 1, psprintf("a%d", foreach_current_index(e) + 1), false);
		g->targetList = lappend(g->targetList, te);
		gnames = lappend(gnames, makeString(te->resname));
	}
	g->windowClause = NIL;
	g->hasWindowFuncs = false;
	g->hasAggs = true;
	g->groupClause = list_copy(w->partitionClause);

	gr->rtekind = RTE_SUBQUERY;
	gr->subquery = g;
	gr->inFromCl = true;
	gr->eref = makeAlias("g", gnames);
	/* the arrays unnested together (a ROWS FROM of one unnest each) with the ordinal as the last column */
	foreach_ptr(Expr, e, cols)
	{
		Oid			t = exprType((Node *) e);
		Oid			at = get_array_type(t);

		rf = makeNode(RangeTblFunction);
		rf->funcexpr = (Node *) makeFuncExpr(F_UNNEST_ANYARRAY, t, list_make1(makeVar(1, nkeys + foreach_current_index(e) + 1, at, -1, InvalidOid, 0)), exprCollation((Node *) e), InvalidOid, COERCE_EXPLICIT_CALL);
		((FuncExpr *) rf->funcexpr)->funcretset = true;
		rf->funccolcount = 1;
		rf->funccolnames = list_make1(makeString(psprintf("c%d", foreach_current_index(e) + 1)));
		rf->funccoltypes = list_make1_oid(t);
		rf->funccoltypmods = list_make1_int(-1);
		rf->funccolcollations = list_make1_oid(exprCollation((Node *) e));
		funcs = lappend(funcs, rf);
		unames = lappend(unames, makeString(psprintf("c%d", foreach_current_index(e) + 1)));
	}
	if (kind == SIFT_TOPN_ROWS)
		unames = lappend(unames, makeString("rn"));
	ur->rtekind = RTE_FUNCTION;
	ur->functions = funcs;
	ur->funcordinality = kind == SIFT_TOPN_ROWS;
	ur->lateral = true;
	ur->inFromCl = true;
	ur->eref = makeAlias("u", unames);

	foreach_node(TargetEntry, te, sub->targetList)
	{
		Expr	   *v;
		int			at;

		TargetEntry *t;

		if (te->resjunk && kind == SIFT_TOPN_ROWS)
			continue;
		if (te == rn && kind != SIFT_TOPN_ROWS)
		{
			/* rank and dense_rank of the rows the scan kept: the window of the query over them, whose rows are all that are needed */
			tlist = lappend(tlist, copyObject(te));
			continue;
		}
		if (te == rn)
			v = (Expr *) makeVar(2, list_length(cols) + 1, INT8OID, -1, InvalidOid, 0);
		else if ((at = expr_index(keys, (Node *) te->expr)) >= 0)
			v = (Expr *) makeVar(1, at + 1, exprType((Node *) te->expr), exprTypmod((Node *) te->expr), exprCollation((Node *) te->expr), 0);
		else
		{
			at = expr_index(cols, (Node *) te->expr);
			v = (Expr *) makeVar(2, at + 1, exprType((Node *) te->expr), -1, exprCollation((Node *) te->expr), 0);
			/* an element of an array has no modifier: the column keeps its own (numeric(12,2)) in the result */
			if (exprTypmod((Node *) te->expr) != -1)
				v = (Expr *) makeRelabelType(v, exprType((Node *) te->expr), exprTypmod((Node *) te->expr), exprCollation((Node *) te->expr), COERCE_IMPLICIT_CAST);
		}
		t = makeTargetEntry(v, te->resno, te->resname, te->resjunk);
		t->ressortgroupref = kind == SIFT_TOPN_ROWS ? 0 : te->ressortgroupref;
		tlist = lappend(tlist, t);
	}
	top->commandType = CMD_SELECT;
	top->canSetTag = true;
	top->rtable = list_make2(gr, ur);
	top->jointree = makeFromExpr(list_make2(makeNode(RangeTblRef), makeNode(RangeTblRef)), NULL);
	linitial_node(RangeTblRef, top->jointree->fromlist)->rtindex = 1;
	lsecond_node(RangeTblRef, top->jointree->fromlist)->rtindex = 2;
	top->targetList = tlist;
	if (kind != SIFT_TOPN_ROWS)
	{
		top->windowClause = list_make1(copyObject(w));
		top->hasWindowFuncs = true;
	}
	return top;
}

#define LATEST_REFUSE(report, text) \
	do { \
		if (report) \
			agg_report("latest", AGG_REFUSE_WINDOW_FIRST, text, false, 0, 0); \
		return false; \
	} while (0)

/* whether the WHERE of q keeps the first row of the window of its sub-select rti; apply rewrites the sub-select, the other pass reports why not */
static bool
latest_of(Query *q, Index rti, bool apply)
{
	RangeTblEntry *rte = rt_fetch(rti, q->rtable);
	Query	   *sub;
	List	   *quals = NIL,
			   *firsts = NIL;
	CommonTableExpr *cte = NULL;
	Query	   *top = NULL;
	int64		n = 0;
	AttrNumber	attno = 0;
	TargetEntry *rn;
	WindowFunc *wf;
	WindowClause *w;
	int			count = 0,
				kind;

	if (rte->rtekind == RTE_SUBQUERY)
		sub = rte->subquery;
	else if (rte->rtekind == RTE_CTE && rte->ctelevelsup == 0)
	{
		sub = NULL;
		foreach_node(CommonTableExpr, c, q->cteList)
			if (strcmp(c->ctename, rte->ctename) == 0 && c->cterefcount == 1 && !c->cterecursive &&
				c->ctematerialized != CTEMaterializeAlways)
			{
				cte = c;
				sub = castNode(Query, c->ctequery);
			}
	}
	else
		return false;
	if (sub == NULL || !sub->hasWindowFuncs || q->jointree->quals == NULL)
		return false;

	quals = make_ands_implicit((Expr *) q->jointree->quals);
	foreach_ptr(Node, qual, quals)
	{
		AttrNumber	a;
		int64		b = row_bound(qual, rti, &a);

		if (b > 0 && (attno == 0 || a == attno))
		{
			attno = a;
			firsts = lappend(firsts, qual);
			n = n == 0 ? b : Min(n, b);
		}
	}
	if (firsts == NIL || attno < 1 || attno > list_length(sub->targetList))
		return false;
	rn = list_nth_node(TargetEntry, sub->targetList, attno - 1);
	if (!IsA(rn->expr, WindowFunc))
		return false;
	wf = (WindowFunc *) rn->expr;

	kind = wf->winfnoid == F_ROW_NUMBER ? SIFT_TOPN_ROWS : wf->winfnoid == F_RANK_ ? SIFT_TOPN_RANK : wf->winfnoid == F_DENSE_RANK_ ? SIFT_TOPN_DENSE : -1;
	if (kind < 0)
		LATEST_REFUSE(!apply, psprintf("%s is not row_number, rank or dense_rank", get_func_name(wf->winfnoid)));
	if (kind != SIFT_TOPN_ROWS && n > SIFT_AGG_TOPN_MAX)
		LATEST_REFUSE(!apply, "rank or dense_rank of more than 1,000 rows");
	count_window_funcs((Node *) sub->targetList, &count);
	if (count != 1 || list_length(sub->windowClause) != 1)
		LATEST_REFUSE(!apply, "the sub-select has another window function");
	w = linitial_node(WindowClause, sub->windowClause);
	if (w->partitionClause == NIL || w->orderClause == NIL)
		LATEST_REFUSE(!apply, "the window has no PARTITION BY or no ORDER BY");
	if (rte->lateral || sub->commandType != CMD_SELECT || sub->setOperations != NULL || sub->hasAggs || sub->hasTargetSRFs || sub->hasForUpdate ||
		sub->groupClause != NIL || sub->groupingSets != NIL || sub->havingQual != NULL || sub->distinctClause != NIL || sub->sortClause != NIL ||
		sub->limitOffset != NULL || sub->limitCount != NULL || sub->rowMarks != NIL || wf->aggfilter != NULL)
		LATEST_REFUSE(!apply, "the sub-select groups, sorts, limits, locks rows or filters the window function");
	if (contain_volatile_functions((Node *) sub))
		LATEST_REFUSE(!apply, "the sub-select calls a volatile function");
	if (n > 1 || kind != SIFT_TOPN_ROWS)
	{
		const char *why;

		bool		indexed = false;

		/* the arrays are for the scan: a sub-select of another sub-select's groups would only trade the window for a hash aggregate */
		foreach_node(RangeTblEntry, r, sub->rtable)
			indexed |= r->rtekind == RTE_RELATION && r->relkind == RELKIND_RELATION && has_column_source(r->relid);
		if (!indexed)
			return false;
		if (sub->hasSubLinks)
			LATEST_REFUSE(!apply, "the sub-select has a sub-select of its own");
		/* the arrays hold every column the sub-select reads, which the scan reads from the index only: planning the rewrite for a column no index holds is a plan to throw away */
		if (list_length(sub->rtable) == 1 && ((RangeTblEntry *) linitial(sub->rtable))->rtekind == RTE_RELATION)
		{
			Bitmapset  *cols = NULL;

			pull_varattnos((Node *) sub->targetList, 1, &cols);
			pull_varattnos((Node *) sub->jointree->quals, 1, &cols);
			if (!column_source_reads(((RangeTblEntry *) linitial(sub->rtable))->relid, cols))
				LATEST_REFUSE(!apply, "a column the sub-select reads is in no column index of its table");
		}
		if ((top = topn_array(sub, w, rn, n, kind, &why)) == NULL)
			LATEST_REFUSE(!apply, why);
	}
	if (!apply)
		return true;

	if (top != NULL)
	{
		/* the plan is chosen by cost: the scan keeps the first rows of each group, PostgreSQL's plan sorts every row of them */
		sift_agg_by_cost = true;
		if (cte != NULL)
			cte->ctequery = (Node *) top;
		else
			rte->subquery = top;
	}
	else
	{
		rn->expr = (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(1), false, FLOAT8PASSBYVAL);
		sub->hasWindowFuncs = false;
		sub->windowClause = NIL;
		sub->hasDistinctOn = true;
		sub->distinctClause = list_copy(w->partitionClause);
		sub->sortClause = list_copy(w->partitionClause);
		foreach_node(SortGroupClause, o, w->orderClause)
			if (get_sortgroupref_clause_noerr(o->tleSortGroupRef, sub->sortClause) == NULL)
				sub->sortClause = lappend(sub->sortClause, o);
	}
	/* rank and dense_rank are computed above the rows the scan kept, which the condition then cuts */
	if (kind == SIFT_TOPN_ROWS)
	{
		foreach_ptr(Node, qual, firsts)
			quals = list_delete_ptr(quals, qual);
		q->jointree->quals = quals == NIL ? NULL : (Node *) make_ands_explicit(quals);
	}
	return true;
}

/* the sub-selects that stand in the FROM list or under the inner joins of q (a condition of the WHERE is one of every row they leave) */
static bool
latest_in(Query *q, Node *n, bool apply)
{
	if (IsA(n, RangeTblRef))
		return latest_of(q, ((RangeTblRef *) n)->rtindex, apply);
	if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER)
	{
		bool		left = latest_in(q, ((JoinExpr *) n)->larg, apply);

		return latest_in(q, ((JoinExpr *) n)->rarg, apply) || left;
	}
	return false;
}

bool
sift_agg_latest_per_group(Query *q, bool apply)
{
	bool		changed = false;

	if (q->jointree == NULL)
		return false;
	foreach_ptr(Node, item, q->jointree->fromlist)
		changed |= latest_in(q, item, apply);
	return changed;
}
