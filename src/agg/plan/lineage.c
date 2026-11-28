/*
 * agg/plan/lineage.c - the conjuncts of an IN sub-select of an inlined shared CTE that the CTE it reads keeps its rows by (agg/plan/lineage.h).
 *
 * The shape: a list CTE `ids` (the ids of a table that satisfy Q), a CTE named as the table that joins the table to `ids`, and a shared CTE whose sub-select
 * `SELECT x.id FROM <that CTE> WHERE Q` restates Q. The rows of the CTE satisfy Q by lineage, so Q is redundant there, and the scope table is read once by the list
 * instead of once more for each aggregate. A conjunct is dropped when the CTE or sub-select it reads keeps its rows by the same condition, directly or through an
 * inner-join equality with a column of a CTE or sub-select that does (followed to four levels, CTE names resolved by their level). The condition must be stable,
 * read one column of an exact type, and read no outer column and no CTE; outer joins, DISTINCT ON, LIMIT, windows, aggregates and set operations keep it.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/plan/lineage.h"

/* the types whose equal values are the same value: a condition on one holds for the other */
static bool
exact_key_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID || t == UUIDOID;
}

/* does the tree hold a Var of a level above the sub-select it is walked as? depth: the sub-selects below it */
static bool
outer_var_walker(Node *n, int *depth)
{
	bool		found;

	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup > *depth;
	if (!IsA(n, Query))
		return expression_tree_walker(n, outer_var_walker, depth);
	(*depth)++;
	found = query_tree_walker((Query *) n, outer_var_walker, depth, 0);
	(*depth)--;
	return found;
}

/* does the tree, or a sub-select of it, read a CTE? */
static bool
cte_ref_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_CTE;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, cte_ref_walker, context, QTW_EXAMINE_RTES_BEFORE);
	return expression_tree_walker(n, cte_ref_walker, context);
}

/* a filter of rows, nothing more: the output rows are rows of the FROM kept by the WHERE (a plain DISTINCT drops duplicates only) */
static bool
plain_filter(const Query *q)
{
	return q->commandType == CMD_SELECT && q->setOperations == NULL && q->cteList == NIL && q->jointree != NULL && !q->hasAggs && !q->hasWindowFuncs &&
		!q->hasTargetSRFs && !q->hasDistinctOn && q->groupClause == NIL && q->groupingSets == NIL && q->limitCount == NULL && q->limitOffset == NULL &&
		q->rowMarks == NIL;
}

typedef struct KeyWalk
{
	Var		   *key;			/* the one column the condition reads */
	bool		ok;
} KeyWalk;

static Node *
key_mutator(Node *n, KeyWalk *w)
{
	int			depth = 0;

	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;
		Var		   *placeholder = copyObject(v);

		if (v->varlevelsup != 0 || (w->key != NULL && (v->varno != w->key->varno || v->varattno != w->key->varattno)))
			w->ok = false;
		else if (w->key == NULL)
			w->key = v;
		placeholder->varno = 1;
		placeholder->varattno = 1;
		return (Node *) placeholder;
	}
	if (IsA(n, Query))
	{
		/* a sub-select is the same condition when it reads the same tables wherever it stands: no column of the query around it, no CTE (the name could be another CTE there) */
		if (query_tree_walker((Query *) n, outer_var_walker, &depth, 0) || cte_ref_walker(n, NULL))
			w->ok = false;
		return n;
	}
	return expression_tree_mutator(n, key_mutator, w);
}

/* the condition as a function of its one column, to compare with the same condition on another column; NULL when it reads other columns or is volatile */
static Node *
key_condition(Node *cond, Var **key)
{
	KeyWalk		w = {*key, true};
	Node	   *norm;

	if (contain_volatile_functions(cond))
		return NULL;
	norm = key_mutator(cond, &w);
	if (!w.ok || w.key == NULL)
		return NULL;
	*key = w.key;
	return norm;
}

/* the conditions an inner join tree applies to its rows; false with an outer join in it (its null-extended rows pass no condition) */
static bool
inner_quals(Node *jt, List **quals)
{
	if (IsA(jt, RangeTblRef))
		return true;
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;

		foreach_ptr(Node, item, f->fromlist)
			if (!inner_quals(item, quals))
				return false;
		*quals = list_concat(*quals, make_ands_implicit((Expr *) f->quals));
		return true;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		if (j->jointype != JOIN_INNER || !inner_quals(j->larg, quals) || !inner_quals(j->rarg, quals))
			return false;
		*quals = list_concat(*quals, make_ands_implicit((Expr *) j->quals));
		return true;
	}
	return false;
}

/* the columns that an equality of the quals ties to the columns of same, every row long */
static void
equal_columns(List **same, List *quals)
{
	for (bool grown = true; grown;)
	{
		grown = false;
		foreach_ptr(Node, qual, quals)
		{
			OpExpr	   *op = (OpExpr *) qual;
			Var		   *a,
					   *b;

			if (!IsA(qual, OpExpr) || list_length(op->args) != 2 || !op_mergejoinable(op->opno, exprType(linitial(op->args))))
				continue;
			a = (Var *) linitial(op->args);
			b = (Var *) lsecond(op->args);
			if (!IsA(a, Var) || !IsA(b, Var) || a->varlevelsup != 0 || b->varlevelsup != 0 || !exact_key_type(a->vartype) || !exact_key_type(b->vartype))
				continue;
			if (list_member(*same, a) != list_member(*same, b))
			{
				*same = lappend(*same, list_member(*same, a) ? b : a);
				grown = true;
			}
		}
	}
}

static bool key_filtered(List *ctes, Query *q, int level, AttrNumber col, Node *want, int depth);

/*
 * Does every row of the column col of the entry satisfy want? level: the ctelevelsup that the CTEs of ctes (the WITH the entry is read in, those before the reading CTE) have
 * for the query that holds the entry. The entry is a CTE of ctes, or a sub-select (a CTE that PostgreSQL would inline is one already).
 */
static bool
rte_filtered(List *ctes, RangeTblEntry *rte, int level, AttrNumber col, Node *want, int depth)
{
	int			pos = 0;

	if (rte->rtekind == RTE_SUBQUERY)
		return key_filtered(ctes, rte->subquery, level + 1, col, want, depth);
	if (rte->rtekind != RTE_CTE || rte->ctelevelsup != (Index) level)
		return false;
	foreach_node(CommonTableExpr, p, ctes)
	{
		if (strcmp(p->ctename, rte->ctename) == 0)
			return IsA(p->ctequery, Query) && key_filtered(list_copy_head(ctes, pos), castNode(Query, p->ctequery), 1, col, want, depth);
		pos++;
	}
	return false;
}

/*
 * Does every row of the column col of the query satisfy want (a key_condition)? It does when the query keeps its rows by that condition on the column, or on a column equal
 * to it, or by an equality with a column of an entry whose rows satisfy it. depth bounds the entries followed.
 */
static bool
key_filtered(List *ctes, Query *q, int level, AttrNumber col, Node *want, int depth)
{
	TargetEntry *tle;
	List	   *quals = NIL;
	List	   *same;

	if (depth <= 0 || !plain_filter(q) || col > list_length(q->targetList) || !inner_quals((Node *) q->jointree, &quals))
		return false;
	tle = list_nth_node(TargetEntry, q->targetList, col - 1);
	if (tle->resjunk || !IsA(tle->expr, Var) || ((Var *) tle->expr)->varlevelsup != 0)
		return false;
	same = list_make1(tle->expr);
	equal_columns(&same, quals);
	foreach_ptr(Var, v, same)
	{
		foreach_ptr(Node, qual, quals)
		{
			Var		   *key = v;
			Node	   *have = key_condition(qual, &key);

			if (have != NULL && equal(have, want))
				return true;
		}
		if (rte_filtered(ctes, rt_fetch(v->varno, q->rtable), level, v->varattno, want, depth - 1))
			return true;
	}
	return false;
}

bool
sift_agg_cte_drop_restated(SubLink *sl, List *ctes)
{
	Query	   *sub;
	RangeTblRef *ref;
	RangeTblEntry *rte;
	List	   *keep = NIL;
	bool		dropped = false;

	if (sl->subLinkType != ANY_SUBLINK || !IsA(sl->subselect, Query))
		return false;
	sub = castNode(Query, sl->subselect);
	if (!plain_filter(sub) || list_length(sub->jointree->fromlist) != 1 || !IsA(linitial(sub->jointree->fromlist), RangeTblRef))
		return false;
	ref = linitial_node(RangeTblRef, sub->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, sub->rtable);
	foreach_ptr(Node, cond, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		Var		   *key = NULL;
		Node	   *want = key_condition(cond, &key);

		if (want != NULL && key->varno == (int) ref->rtindex && rte_filtered(ctes, rte, 2, key->varattno, want, 4))
			dropped = true;
		else
			keep = lappend(keep, cond);
	}
	if (dropped)
		sub->jointree->quals = keep == NIL ? NULL : (Node *) make_ands_explicit(keep);
	return dropped;
}
