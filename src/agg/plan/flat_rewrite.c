/*
 * agg/plan/flat_rewrite.c - rewrites of a statement before it is planned, each giving the rows of the form it replaces, that leave the aggregate scan a statement it takes:
 * - a HAVING `key IN (sub-select)` or EXISTS of a plain GROUP BY that reads no aggregate is a condition of the rows (every row of a group has its key): it moves to WHERE, where the
 *   planner makes a semi join of it that the scan answers, where a HAVING runs it as a sub-plan once for each group. PostgreSQL moves the HAVING conditions without a sub-select
 *   itself (subquery_planner). Grouping sets stay (a key is NULL in the rows of a super-aggregate), and so does a HAVING with no GROUP BY (its one group is there when no row is);
 * - a LEFT JOIN LATERAL (.. LIMIT 1) that nothing reads keeps every row of its left side once: the join is its left side. The planner does not see it, its join removal needs the
 *   right side unique on the join conditions and `ON true` gives none;
 * - a scalar sub-select of one aggregate over one table tied to the query by equalities of columns, `(SELECT max(b.v) FROM t b WHERE b.k = a.k)`, is a LEFT JOIN of the query to
 *   `t GROUP BY k`: a k has one group at most, a NULL k or a k with no row matches none, and the aggregates taken are NULL over no row as the sub-select's is.
 */
#include "postgres.h"

#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/plan/flat_rewrite.h"

#if PG_VERSION_NUM >= 180000
typedef struct Ungroup
{
	Index		rti;
	List	   *exprs;
	int			depth;
} Ungroup;

static Node *
ungroup_mutator(Node *n, Ungroup *u)
{
	Node	   *r;

	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == u->depth && ((Var *) n)->varno == u->rti)
	{
		r = copyObject(list_nth(u->exprs, ((Var *) n)->varattno - 1));
		IncrementVarSublevelsUp(r, u->depth, 0);
		return r;
	}
	if (!IsA(n, Query))
		return expression_tree_mutator(n, ungroup_mutator, u);
	u->depth++;
	r = (Node *) query_tree_mutator((Query *) n, ungroup_mutator, u, 0);
	u->depth--;
	return r;
}
#endif

/* the expression with the columns of the grouping entry of 18 as the expressions they group by (in a sub-select of it too), as 17 parses it */
static Node *
ungroup_expr(Query *q, Node *n)
{
#if PG_VERSION_NUM >= 180000
	Ungroup		u = {0};

	if (!q->hasGroupRTE)
		return n;
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		u.rti++;
		if (rte->rtekind == RTE_GROUP)
		{
			u.exprs = rte->groupexprs;
			return ungroup_mutator(n, &u);
		}
	}
#endif
	return n;
}

/* EXISTS of a sub-select the planner pulls up as a semi join: any other is a sub-plan run for each row in WHERE */
static bool
semi_subselect(const Query *q)
{
	return !q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs && q->groupClause == NIL && q->havingQual == NULL && q->limitCount == NULL && q->limitOffset == NULL &&
		q->setOperations == NULL && q->cteList == NIL;
}

/*
 * `column IN (sub-select)` with the column as it is (or relabels of it that keep its collation: another cast of it is a function of the key that the rows of a group may not agree
 * on) and a sub-select that does not read the query, or EXISTS of a semi join; no aggregate of this query and nothing volatile. NOT IN, NOT EXISTS and an expression of the key
 * stay on the groups, where the rows form would test every row.
 */
static bool
row_condition(Query *q, Node *c)
{
	SubLink    *sl = (SubLink *) c;
	OpExpr	   *op = (OpExpr *) sl->testexpr;
	Node	   *key;

	if (!IsA(c, SubLink) || contain_aggs_of_level(c, 0) || contain_volatile_functions(c))
		return false;
	if (sl->subLinkType == EXISTS_SUBLINK)
		return semi_subselect(castNode(Query, sl->subselect));
	if (sl->subLinkType != ANY_SUBLINK || !IsA(op, OpExpr) || contain_vars_of_level(sl->subselect, 1))
		return false;
	key = linitial(op->args);
	if (!op_mergejoinable(op->opno, exprType(key)))
		return false;
	key = ungroup_expr(q, key);
	while (IsA(key, RelabelType) && ((RelabelType *) key)->resultcollid == exprCollation((Node *) ((RelabelType *) key)->arg))
		key = (Node *) ((RelabelType *) key)->arg;
	return IsA(key, Var);
}

/* the conditions of the rows of the HAVING moved to WHERE (apply), or whether there is one */
static bool
having_to_where(Query *q, bool apply)
{
	List	   *keep = NIL,
			   *rows = NIL;

	if (q->commandType != CMD_SELECT || q->havingQual == NULL || !q->hasSubLinks || q->groupClause == NIL || q->groupingSets != NIL)
		return false;
	foreach_ptr(Node, c, make_ands_implicit((Expr *) q->havingQual))
	{
		if (row_condition(q, c))
			rows = lappend(rows, apply ? ungroup_expr(q, c) : c);
		else
			keep = lappend(keep, c);
	}
	if (rows == NIL || !apply)
		return rows != NIL;
	q->havingQual = keep == NIL ? NULL : (Node *) make_ands_explicit(keep);
	q->jointree->quals = (Node *) make_ands_explicit(list_concat(make_ands_implicit((Expr *) q->jointree->quals), rows));
	return true;
}

/* at every level of the statement; with apply false the walk stops at the first */
static bool
having_walker(Node *n, bool *apply)
{
	bool		found;

	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (!IsA(n, Query))
		return expression_tree_walker(n, having_walker, apply);
	found = having_to_where((Query *) n, *apply);
	return query_tree_walker((Query *) n, having_walker, apply, 0) || (found && !*apply);
}

typedef struct Reads
{
	Index		rti;			/* the sub-select's entry */
	Index		jrti;			/* the join's entry (0: none); around: the entries of the joins around it, whose aliases hold the sub-select's columns */
	Bitmapset  *around;
	int			level;
	bool		found;
} Reads;

static bool
reads_walker(Node *n, Reads *c)
{
	bool		r;

	if (n == NULL || c->found)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		c->found = v->varlevelsup == c->level && (v->varno == c->rti || (c->jrti != 0 && v->varno == c->jrti) || bms_is_member(v->varno, c->around));
		return false;
	}
	if (!IsA(n, Query))
		return expression_tree_walker(n, reads_walker, c);
	c->level++;
	r = query_tree_walker((Query *) n, reads_walker, c, QTW_IGNORE_JOINALIASES);
	c->level--;
	return r;
}

/* does anything of the query but the join's own condition read the sub-select, or a join around it; the range table too: a later lateral sub-select reads it from its own entry */
static bool
is_read(Query *q, JoinExpr *j, Index rti, Bitmapset *around)
{
	Reads		c = {rti, j->rtindex, around, 0, false};
	Node	   *quals = j->quals;

	j->quals = NULL;
	query_tree_walker(q, reads_walker, &c, QTW_IGNORE_JOINALIASES);
	j->quals = quals;
	return c.found;
}

/* at most one row, and no effect but its rows: LIMIT 1 (not WITH TIES), no volatile function, no row lock, no data-modifying CTE */
static bool
one_row_query(Query *sq)
{
	Const	   *limit;

	if (sq->commandType != CMD_SELECT || sq->limitOption != LIMIT_OPTION_COUNT || sq->limitCount == NULL || sq->rowMarks != NIL || sq->hasModifyingCTE ||
		contain_volatile_functions((Node *) sq))
		return false;
	limit = (Const *) eval_const_expressions(NULL, sq->limitCount);
	return IsA(limit, Const) && !limit->constisnull && limit->consttype == INT8OID && DatumGetInt64(limit->constvalue) == 1;
}

static Node *
drop_in(Query *q, Node *jt, Bitmapset *around)
{
	JoinExpr   *j = (JoinExpr *) jt;
	Bitmapset  *inner;
	RangeTblEntry *rte;

	if (IsA(jt, FromExpr))
	{
		ListCell   *lc;

		foreach(lc, ((FromExpr *) jt)->fromlist)
			lfirst(lc) = drop_in(q, lfirst(lc), around);
		return jt;
	}
	if (!IsA(jt, JoinExpr))
		return jt;
	inner = j->rtindex != 0 ? bms_add_member(bms_copy(around), j->rtindex) : around;
	j->larg = drop_in(q, j->larg, inner);
	j->rarg = drop_in(q, j->rarg, inner);
	if (j->jointype != JOIN_LEFT || !IsA(j->rarg, RangeTblRef))
		return jt;
	rte = rt_fetch(((RangeTblRef *) j->rarg)->rtindex, q->rtable);
	if (rte->rtekind != RTE_SUBQUERY || !one_row_query(rte->subquery) || is_read(q, j, ((RangeTblRef *) j->rarg)->rtindex, around))
		return jt;
	/* the planner counts the outer joins of the range table and expects each in the join tree */
	if (j->rtindex != 0)
		rt_fetch(j->rtindex, q->rtable)->jointype = JOIN_INNER;
	return j->larg;
}

static void drop_level(Query *q);

static bool
drop_sublinks(Node *n, void *cx)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		drop_level((Query *) n);
		return false;
	}
	return expression_tree_walker(n, drop_sublinks, cx);
}

/* the arms of a set operation stay alike (the two-level scan joins arms of one shape), and so do a security_barrier view and a statement that locks rows */
static void
drop_level(Query *q)
{
	if (q->commandType != CMD_SELECT || q->rowMarks != NIL)
		return;
	foreach_node(CommonTableExpr, cte, q->cteList)
		drop_level(castNode(Query, cte->ctequery));
	if (q->setOperations == NULL)
		foreach_node(RangeTblEntry, rte, q->rtable)
			if (rte->rtekind == RTE_SUBQUERY && !rte->security_barrier)
				drop_level(rte->subquery);
	if (q->hasSubLinks)
		query_tree_walker(q, drop_sublinks, NULL, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
	q->jointree = (FromExpr *) drop_in(q, (Node *) q->jointree, NULL);
}

void
sift_agg_drop_unused_laterals(Query *q)
{
	drop_level(q);
}

/* an aggregate of pg_catalog whose value over no row is NULL, as a LEFT JOIN gives for a key with no group */
static bool
null_on_empty(const Aggref *a)
{
	static const char *const names[] = {"sum", "avg", "min", "max", "bool_and", "bool_or", "string_agg", "array_agg"};
	char	   *name = get_func_name(a->aggfnoid);

	if (a->agglevelsup != 0 || a->aggkind != AGGKIND_NORMAL || name == NULL || get_func_namespace(a->aggfnoid) != PG_CATALOG_NAMESPACE || contain_vars_of_level((Node *) a, 1))
		return false;
	for (int i = 0; i < lengthof(names); i++)
		if (strcmp(name, names[i]) == 0)
			return true;
	return false;
}

/* `table.column = outer column` of one type, by the equality that groups the type */
static bool
key_equality(Node *c, Var **in, Var **out)
{
	OpExpr	   *op = (OpExpr *) c;
	Var		   *l,
			   *r;

	if (!IsA(c, OpExpr) || list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	*in = l->varlevelsup == 0 ? l : r;
	*out = l->varlevelsup == 0 ? r : l;
	return (*in)->varlevelsup == 0 && (*in)->varno == 1 && (*in)->varattno > 0 && (*out)->varlevelsup == 1 && (*out)->varattno > 0 && (*in)->vartype == (*out)->vartype &&
		lookup_type_cache((*in)->vartype, TYPECACHE_EQ_OPR)->eq_opr == op->opno;
}

/* a sub-select of one aggregate over one table, no other clause; its WHERE split into the equalities that tie it to the query and the rest, which reads only the table */
static bool
decor_subselect(const SubLink *sl, List **eqs, List **rest)
{
	Query	   *s = (Query *) sl->subselect;
	TargetEntry *te;

	if (sl->subLinkType != EXPR_SUBLINK || s->commandType != CMD_SELECT || list_length(s->rtable) != 1 || rt_fetch(1, s->rtable)->rtekind != RTE_RELATION ||
		list_length(s->jointree->fromlist) != 1 || !IsA(linitial(s->jointree->fromlist), RangeTblRef) || !s->hasAggs || s->groupClause != NIL || s->groupingSets != NIL ||
		s->havingQual != NULL || s->hasWindowFuncs || s->hasTargetSRFs || s->hasSubLinks || s->distinctClause != NIL || s->limitCount != NULL || s->limitOffset != NULL ||
		s->setOperations != NULL || s->cteList != NIL || s->rowMarks != NIL || list_length(s->targetList) != 1 || contain_volatile_functions((Node *) s))
		return false;
	te = linitial_node(TargetEntry, s->targetList);
	if (!IsA(te->expr, Aggref) || !null_on_empty((Aggref *) te->expr))
		return false;
	*eqs = *rest = NIL;
	foreach_ptr(Node, c, make_ands_implicit((Expr *) s->jointree->quals))
	{
		Var		   *in,
				   *out;

		if (key_equality(c, &in, &out))
			*eqs = lappend(*eqs, c);
		else if (contain_vars_of_level(c, 1))
			return false;
		else
			*rest = lappend(*rest, c);
	}
	return *eqs != NIL;
}

typedef struct Decor
{
	Query	   *q;
	bool		apply;
	bool		changed;
} Decor;

static RangeTblEntry *
join_entry(void)
{
	RangeTblEntry *j = makeNode(RangeTblEntry);

	j->rtekind = RTE_JOIN;
	j->jointype = JOIN_LEFT;
	j->eref = makeAlias("unnamed_join", NIL);
	j->inFromCl = true;
	return j;
}

/* the sub-select grouped by the columns of its equalities, and the LEFT JOIN of the query to it on them, around the join tree of the query; the column of its aggregate */
static Node *
decorrelate(Decor *d, SubLink *sl, List *eqs, List *rest)
{
	Query	   *q = d->q,
			   *g = (Query *) sl->subselect;
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	JoinExpr   *j = makeNode(JoinExpr);
	TargetEntry *agg = linitial_node(TargetEntry, g->targetList);
	List	   *names = list_make1(makeString("agg"));
	List	   *on = NIL;
	Var		   *col;

	g->jointree->quals = (Node *) make_ands_explicit(rest);
	foreach_node(OpExpr, eq, eqs)
	{
		Var		   *in,
				   *out;
		SortGroupClause *sgc = makeNode(SortGroupClause);
		int			resno = list_length(g->targetList) + 1;
		TargetEntry *key;

		key_equality((Node *) eq, &in, &out);
		out = copyObject(out);
		out->varlevelsup = 0;
		get_sort_group_operators(in->vartype, true, true, false, &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
		sgc->tleSortGroupRef = resno;
		key = makeTargetEntry((Expr *) copyObject(in), resno, psprintf("k%d", resno), false);
		key->ressortgroupref = resno;
		g->targetList = lappend(g->targetList, key);
		g->groupClause = lappend(g->groupClause, sgc);
		names = lappend(names, makeString(psprintf("k%d", resno)));
		on = lappend(on, make_opclause(eq->opno, BOOLOID, false, (Expr *) makeVar(list_length(q->rtable) + 1, resno, in->vartype, in->vartypmod, in->varcollid, 0),
									   (Expr *) out, InvalidOid, eq->inputcollid));
	}
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = g;
	rte->alias = makeAlias("sift_decor", NIL);
	rte->eref = makeAlias("sift_decor", names);
	rte->inFromCl = true;
	q->rtable = lappend(q->rtable, rte);
	q->rtable = lappend(q->rtable, join_entry());
	j->jointype = JOIN_LEFT;
	j->larg = list_length(q->jointree->fromlist) == 1 ? linitial(q->jointree->fromlist) : (Node *) makeFromExpr(q->jointree->fromlist, NULL);
	j->rarg = (Node *) makeNode(RangeTblRef);
	((RangeTblRef *) j->rarg)->rtindex = list_length(q->rtable) - 1;
	j->quals = (Node *) make_ands_explicit(on);
	j->rtindex = list_length(q->rtable);
	q->jointree->fromlist = list_make1(j);
	col = makeVar(list_length(q->rtable) - 1, 1, exprType((Node *) agg->expr), exprTypmod((Node *) agg->expr), exprCollation((Node *) agg->expr), 0);
	col->varnullingrels = bms_make_singleton(j->rtindex);
	d->changed = true;
	return (Node *) col;
}

static Node *
decor_mutator(Node *n, Decor *d)
{
	List	   *eqs,
			   *rest;

	if (n == NULL || IsA(n, Query))
		return n;
	if (IsA(n, SubLink) && decor_subselect((SubLink *) n, &eqs, &rest))
		return d->apply ? decorrelate(d, (SubLink *) n, eqs, rest) : (d->changed = true, n);
	return expression_tree_mutator(n, decor_mutator, d);
}

static bool
decorrelate_tlist(Query *q, bool apply)
{
	Decor		d = {q, apply, false};
	List	   *tlist;

	if (!q->hasSubLinks || q->hasAggs || q->groupClause != NIL || q->groupingSets != NIL || q->hasWindowFuncs || q->hasTargetSRFs || q->setOperations != NULL ||
		q->jointree->fromlist == NIL)
		return false;
	tlist = (List *) decor_mutator((Node *) q->targetList, &d);
	if (apply)
		q->targetList = tlist;
	return d.changed;
}

bool
sift_agg_having_to_where(Query *q, bool apply)
{
	return q->commandType == CMD_SELECT && q->rowMarks == NIL && having_walker((Node *) q, &apply);
}

bool
sift_agg_flat_decorrelate(Query *q, bool apply)
{
	return q->commandType == CMD_SELECT && q->rowMarks == NIL && decorrelate_tlist(q, apply);
}
