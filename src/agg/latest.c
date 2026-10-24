/*
 * agg/latest.c - the first row of each group, asked with a window function.
 *
 * `SELECT .. FROM (SELECT .., row_number() OVER (PARTITION BY k ORDER BY o) rn FROM t) s WHERE rn = 1` keeps the first row of o for each k. That
 * is `SELECT DISTINCT ON (k) .. FROM t ORDER BY k, o`, which the scan runs in one pass over the rows where the window sorts all of them. The
 * rewrite is made where the query is that one: the sub-select has the one window function, the outer WHERE keeps its first row (`= 1`, `<= 1`,
 * `< 2`), so every row that is left has rn = 1 and the column reads 1 anywhere the query uses it. Rows of a tie in o are in no order in the
 * window and in DISTINCT ON: the row kept is one of the tie in both. The planner hook plans the query both ways, as for the lateral
 * sub-selects of flat.c, and keeps the rewritten one only if it runs in the scan.
 */
#include "postgres.h"

#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/refuse.h"

/* `rn = 1`, `rn <= 1` or `rn < 2` (or the same with the operands swapped) on a column of the range table entry rti */
static bool
keeps_first_row(Node *qual, Index rti, AttrNumber *attno)
{
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *c;
	const char *name;
	int64		n;
	bool		swapped;

	if (!IsA(qual, OpExpr) || list_length((op = (OpExpr *) qual)->args) != 2 || get_func_namespace(op->opfuncid) != PG_CATALOG_NAMESPACE)
		return false;
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
		return false;
	c = (Const *) r;
	if (c->constisnull)
		return false;
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
			return false;
	}
	name = get_opname(op->opno);
	if (name == NULL || !(strcmp(name, "=") == 0 ? n == 1 :
						  strcmp(name, swapped ? ">=" : "<=") == 0 ? n == 1 :
						  strcmp(name, swapped ? ">" : "<") == 0 && n == 2))
		return false;
	*attno = ((Var *) l)->varattno;
	return true;
}

static bool
count_window_funcs(Node *n, int *count)
{
	if (n == NULL)
		return false;
	if (IsA(n, WindowFunc))
		(*count)++;
	return expression_tree_walker(n, count_window_funcs, count);
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
	AttrNumber	attno = 0;
	TargetEntry *rn;
	WindowFunc *wf;
	WindowClause *w;
	int			count = 0;

	if (rte->rtekind == RTE_SUBQUERY)
		sub = rte->subquery;
	else if (rte->rtekind == RTE_CTE && rte->ctelevelsup == 0)
	{
		sub = NULL;
		foreach_node(CommonTableExpr, cte, q->cteList)
			if (strcmp(cte->ctename, rte->ctename) == 0 && cte->cterefcount == 1 && !cte->cterecursive &&
				cte->ctematerialized != CTEMaterializeAlways)
				sub = castNode(Query, cte->ctequery);
	}
	else
		return false;
	if (sub == NULL || !sub->hasWindowFuncs || q->jointree->quals == NULL)
		return false;

	quals = make_ands_implicit((Expr *) q->jointree->quals);
	foreach_ptr(Node, qual, quals)
	{
		AttrNumber	a;

		if (keeps_first_row(qual, rti, &a) && (attno == 0 || a == attno))
		{
			attno = a;
			firsts = lappend(firsts, qual);
		}
	}
	if (firsts == NIL || attno < 1 || attno > list_length(sub->targetList))
		return false;
	rn = list_nth_node(TargetEntry, sub->targetList, attno - 1);
	if (!IsA(rn->expr, WindowFunc))
		return false;
	wf = (WindowFunc *) rn->expr;

	if (wf->winfnoid != F_ROW_NUMBER)
		LATEST_REFUSE(!apply, psprintf("%s is not row_number", get_func_name(wf->winfnoid)));
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
	if (!apply)
		return true;

	rn->expr = (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(1), false, FLOAT8PASSBYVAL);
	sub->hasWindowFuncs = false;
	sub->windowClause = NIL;
	sub->hasDistinctOn = true;
	sub->distinctClause = list_copy(w->partitionClause);
	sub->sortClause = list_copy(w->partitionClause);
	foreach_node(SortGroupClause, o, w->orderClause)
		if (get_sortgroupref_clause_noerr(o->tleSortGroupRef, sub->sortClause) == NULL)
			sub->sortClause = lappend(sub->sortClause, o);
	foreach_ptr(Node, qual, firsts)
		quals = list_delete_ptr(quals, qual);
	q->jointree->quals = quals == NIL ? NULL : (Node *) make_ands_explicit(quals);
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
