/*
 * agg/plan/flat_rewrite.c - rewrites of a statement before it is planned, each giving the rows of the form it replaces, that leave the aggregate scan a statement it takes:
 * - a HAVING `key IN (sub-select)` or EXISTS of a plain GROUP BY that reads no aggregate is a condition of the rows (every row of a group has its key): it moves to WHERE, where the
 *   planner makes a semi join of it that the scan answers, where a HAVING runs it as a sub-plan once for each group. PostgreSQL moves the HAVING conditions without a sub-select
 *   itself (subquery_planner). Grouping sets stay (a key is NULL in the rows of a super-aggregate), and so does a HAVING with no GROUP BY (its one group is there when no row is);
 * - a LEFT JOIN LATERAL (.. LIMIT 1) that nothing reads keeps every row of its left side once: the join is its left side. The planner does not see it, its join removal needs the
 *   right side unique on the join conditions and `ON true` gives none;
 * - a LATERAL (SELECT exprs [WHERE gate] LIMIT 1) without FROM gives one row of exprs for each row of its left side (none or nulls where the gate fails): its columns are the exprs, the gate a
 *   condition of the query (CASE WHEN gate THEN expr END under a LEFT JOIN). An expr that may raise moves only where it runs for the same rows as the sub-select's;
 * - a scalar sub-select of one aggregate over one table tied to the query by equalities of columns, `(SELECT max(b.v) FROM t b WHERE b.k = a.k)`, is a LEFT JOIN of the query to
 *   `t GROUP BY k`: a k has one group at most, a NULL k or a k with no row matches none, and the aggregates taken are NULL over no row as the sub-select's is. An EXISTS of the select
 *   list tied the same way is the LEFT JOIN to `t GROUP BY k` and whether the outer row found a group;
 * - a LATERAL sub-select of aggregates of one table tied the same way, the last item of the FROM list (`x, LATERAL (..)`, `x [LEFT] JOIN LATERAL (..) ON true`), is the join to the table
 *   grouped by its groups and k: an inner join keeps the groups of the outer row, and one row of aggregates for every outer row is a LEFT JOIN, as above.
 */
#include "postgres.h"

#include "access/nbtree.h"
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
#include "agg/safety.h"

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

/* the expression without the relabels that keep its collation: another cast of it is a function of the key that the rows of a group may not agree on */
static Node *
strip_key(Node *n)
{
	while (IsA(n, RelabelType) && ((RelabelType *) n)->resultcollid == exprCollation((Node *) ((RelabelType *) n)->arg))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* the grouping clause of the key that is the column v (the key as it is or relabeled) */
static SortGroupClause *
key_clause(Query *q, const Var *v)
{
	foreach_node(SortGroupClause, sgc, q->groupClause)
	{
		Node	   *e = strip_key(ungroup_expr(q, get_sortgroupclause_expr(sgc, q->targetList)));

		if (IsA(e, Var) && ((Var *) e)->varno == v->varno && ((Var *) e)->varattno == v->varattno)
			return sgc;
	}
	return NULL;
}

/* is op an equality the rows of a group agree by on the key column v: one of the family of the key's, in the key's collation (citext's, not text's) */
static bool
group_equality(Query *q, const OpExpr *op, const Var *v)
{
	SortGroupClause *sgc = key_clause(q, v);

	return sgc != NULL && equality_ops_are_compatible(op->opno, sgc->eqop) && op->inputcollid == v->varcollid;
}

/* are the rows of a group of the key column v alike in every function of it: its equality is image equality (an int, a text of a deterministic collation; not a numeric's scale) */
static bool
image_key(Query *q, const Var *v)
{
	TypeCacheEntry *tc = lookup_type_cache(v->vartype, TYPECACHE_BTREE_OPFAMILY);
	Oid			proc;

	if (key_clause(q, v) == NULL || !OidIsValid(tc->btree_opf))
		return false;
	proc = get_opfamily_proc(tc->btree_opf, tc->btree_opintype, tc->btree_opintype, BTEQUALIMAGE_PROC);
	return OidIsValid(proc) && DatumGetBool(OidFunctionCall1Coll(proc, v->varcollid, ObjectIdGetDatum(tc->btree_opintype)));
}

typedef struct KeyReads
{
	Query	   *q;
	int			depth;			/* how far the walk is below the query */
} KeyReads;

/* does the sub-select read the query other than as a key alike in every row of a group, or as an operand of an equality of a key: a function of a numeric key (its scale) is not */
static bool
reads_beyond_keys(Node *n, KeyReads *k)
{
	bool		r;

	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup == k->depth && !image_key(k->q, (Var *) n);
	if (IsA(n, OpExpr))
	{
		foreach_ptr(Node, a, ((OpExpr *) n)->args)
		{
			Node	   *key = strip_key(a);

			if (!(IsA(key, Var) && ((Var *) key)->varlevelsup == k->depth && group_equality(k->q, (OpExpr *) n, (Var *) key)) && reads_beyond_keys(a, k))
				return true;
		}
		return false;
	}
	if (!IsA(n, Query))
		return expression_tree_walker(n, reads_beyond_keys, k);
	k->depth++;
	r = query_tree_walker((Query *) n, reads_beyond_keys, k, 0);
	k->depth--;
	return r;
}

/*
 * `column IN (sub-select)` by the equality of the column's key, the column as it is, and a sub-select that does not read the query, or EXISTS of a semi join that reads the query
 * only through equalities of its keys; no aggregate of this query and nothing volatile. NOT IN, NOT EXISTS and an expression of the key stay on the groups, where the rows form
 * would test every row.
 */
static bool
row_condition(Query *q, Node *c)
{
	SubLink    *sl = (SubLink *) c;
	OpExpr	   *op = (OpExpr *) sl->testexpr;
	KeyReads	k = {q, 0};
	Node	   *key;

	if (!IsA(c, SubLink) || contain_aggs_of_level(c, 0) || contain_volatile_functions(c))
		return false;
	if (sl->subLinkType == EXISTS_SUBLINK)
		return semi_subselect(castNode(Query, sl->subselect)) && !reads_beyond_keys(ungroup_expr(q, c), &k);
	if (sl->subLinkType != ANY_SUBLINK || !IsA(op, OpExpr) || contain_vars_of_level(sl->subselect, 1))
		return false;
	key = strip_key(ungroup_expr(q, linitial(op->args)));
	return IsA(key, Var) && group_equality(q, op, (Var *) key);
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

/* the joins of jt with an unread one-row lateral dropped, when nothing of it can raise or has an effect: the normal plan runs its sub-select and ON condition for each left row */
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
	if (rte->rtekind != RTE_SUBQUERY || !one_row_query(rte->subquery) || contain_volatile_functions(j->quals) || agg_node_may_raise(j->quals) ||
		agg_node_may_raise((Node *) rte->subquery) || is_read(q, j, ((RangeTblRef *) j->rarg)->rtindex, around))
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

/* a sub-select of one row without FROM: its select list reads the query a level up, its WHERE (a gate) too */
static bool
fromless_row(RangeTblEntry *rte)
{
	Query	   *s = rte->subquery;

	return rte->rtekind == RTE_SUBQUERY && one_row_query(s) && s->rtable == NIL && s->jointree->fromlist == NIL && !s->hasAggs && !s->hasWindowFuncs &&
		!s->hasTargetSRFs && !s->hasSubLinks && s->groupClause == NIL && s->havingQual == NULL && s->distinctClause == NIL && s->limitOffset == NULL &&
		s->setOperations == NULL && s->cteList == NIL && !agg_node_may_raise(s->jointree->quals);
}

typedef struct Inline
{
	Index		rti;			/* the sub-select's entry */
	List	   *cols;			/* the expressions of its columns in the terms of the query, NULL for a column that is no plain one */
	int			depth;
	bool		whole;			/* a whole-row reference of the entry */
	Bitmapset  *raising;		/* the columns that may raise */
	Bitmapset  *read;			/* the raising columns read, and those read where every row reaching the read evaluates them */
	Bitmapset  *always;
} Inline;

static Node *
inline_mutator(Node *n, Inline *in)
{
	Node	   *r;

	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == in->depth && ((Var *) n)->varno == in->rti)
	{
		r = copyObject(list_nth(in->cols, ((Var *) n)->varattno - 1));
		IncrementVarSublevelsUp(r, in->depth, 0);
		return r;
	}
	if (!IsA(n, Query))
		return expression_tree_mutator(n, inline_mutator, in);
	in->depth++;
	r = (Node *) query_tree_mutator((Query *) n, inline_mutator, in, 0);
	in->depth--;
	return r;
}

/* the columns of the entry that n reads (whole: a whole-row reference), at any level below the query */
static bool
cols_walker(Node *n, Inline *in)
{
	bool		r;

	if (n == NULL)
		return false;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == in->depth && ((Var *) n)->varno == in->rti)
	{
		in->whole |= ((Var *) n)->varattno <= 0;
		if (((Var *) n)->varattno > 0 && bms_is_member(((Var *) n)->varattno, in->raising))
			in->read = bms_add_member(in->read, ((Var *) n)->varattno);
		return false;
	}
	if (!IsA(n, Query))
		return expression_tree_walker(n, cols_walker, in);
	in->depth++;
	r = query_tree_walker((Query *) n, cols_walker, in, 0);
	in->depth--;
	return r;
}

/* the columns of the entry read where every input row evaluates them: down calls that evaluate each argument (no CASE, COALESCE, AND or OR, which may skip one) */
static void
always_read(Node *n, Inline *in)
{
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varno == in->rti && ((Var *) n)->varattno > 0)
		in->always = bms_add_member(in->always, ((Var *) n)->varattno);
	else if (IsA(n, FuncExpr) || IsA(n, OpExpr))
		foreach_ptr(Node, a, IsA(n, FuncExpr) ? ((FuncExpr *) n)->args : ((OpExpr *) n)->args)
			always_read(a, in);
	else if (IsA(n, RelabelType))
		always_read((Node *) ((RelabelType *) n)->arg, in);
	else if (IsA(n, CoerceViaIO))
		always_read((Node *) ((CoerceViaIO *) n)->arg, in);
}

/* the arguments of the aggregates without FILTER of the level, which run for each row of a group */
static bool
agg_args_walker(Node *n, Inline *in)
{
	if (n == NULL || IsA(n, Query))
		return false;
	if (IsA(n, Aggref) && ((Aggref *) n)->agglevelsup == 0)
	{
		if (((Aggref *) n)->aggfilter == NULL)
			foreach_node(TargetEntry, te, ((Aggref *) n)->args)
				always_read((Node *) te->expr, in);
		return false;
	}
	return expression_tree_walker(n, agg_args_walker, in);
}

/*
 * May the sub-select's columns that can raise move to where the query reads them: the lateral is the one other item of the FROM list of a grouped query (the filters of the table run before it
 * in both plans), no condition of the query reads them, and each one read is read by a key or an aggregate of every row, as the sub-select evaluates it for every row that reaches it
 */
static bool
raising_inline_ok(Query *q, Inline *in, JoinExpr *j)
{
	Node	   *left = j->larg;

	if (bms_is_empty(in->raising))
		return true;
	if (list_length(q->jointree->fromlist) != 1 || linitial(q->jointree->fromlist) != j || !IsA(left, RangeTblRef) ||
		rt_fetch(((RangeTblRef *) left)->rtindex, q->rtable)->rtekind != RTE_RELATION || (!q->hasAggs && q->groupClause == NIL) || q->hasWindowFuncs ||
		q->hasTargetSRFs || q->groupingSets != NIL || checkExprHasSubLink(q->jointree->quals))
		return false;
	cols_walker(q->jointree->quals, in);
	cols_walker(j->quals, in);
	if (!bms_is_empty(in->read))
		return false;
	cols_walker((Node *) q->targetList, in);
	cols_walker(q->havingQual, in);
	foreach_node(SortGroupClause, sgc, q->groupClause)
		always_read(ungroup_expr(q, (Node *) get_sortgroupclause_expr(sgc, q->targetList)), in);
	agg_args_walker((Node *) q->targetList, in);
	agg_args_walker(q->havingQual, in);
	return bms_is_subset(in->read, in->always);
}

/* the join at *at of the FROM list of q, or one of the inner joins and left sides below it, whose right side is a one-row sub-select without FROM, inlined (apply) */
static bool
inline_in(Query *q, Node **at, bool apply)
{
	JoinExpr   *j = (JoinExpr *) *at;
	Index		rti;
	RangeTblEntry *rte;
	Query	   *s;
	Inline		in = {0};
	Node	   *gate;

	if (!IsA(*at, JoinExpr) || (j->jointype != JOIN_INNER && j->jointype != JOIN_LEFT))
		return false;
	if (inline_in(q, &j->larg, apply) || (j->jointype == JOIN_INNER && inline_in(q, &j->rarg, apply)))
		return true;
	if (!IsA(j->rarg, RangeTblRef) || !fromless_row(rte = rt_fetch(rti = ((RangeTblRef *) j->rarg)->rtindex, q->rtable)))
		return false;
	if (j->jointype == JOIN_LEFT && j->quals != NULL && !(IsA(j->quals, Const) && !((Const *) j->quals)->constisnull && DatumGetBool(((Const *) j->quals)->constvalue)))
		return false;
	s = rte->subquery;
	gate = s->jointree->quals;
	in.rti = rti;
	foreach_node(TargetEntry, te, s->targetList)
	{
		Node	   *e = copyObject((Node *) te->expr);

		if (agg_node_may_raise(e))
			in.raising = bms_add_member(in.raising, te->resno);
		IncrementVarSublevelsUp(e, -1, 1);
		if (gate != NULL && j->jointype == JOIN_LEFT)
		{
			CaseExpr   *c = makeNode(CaseExpr);
			CaseWhen   *w = makeNode(CaseWhen);

			w->expr = copyObject((Expr *) gate);
			IncrementVarSublevelsUp((Node *) w->expr, -1, 1);
			w->result = (Expr *) e;
			w->location = -1;
			c->casetype = exprType(e);
			c->casecollid = exprCollation(e);
			c->args = list_make1(w);
			c->defresult = (Expr *) makeNullConst(c->casetype, exprTypmod(e), c->casecollid);
			c->location = -1;
			e = (Node *) c;
		}
		in.cols = lappend(in.cols, e);
	}
	query_tree_walker(q, cols_walker, &in, QTW_IGNORE_JOINALIASES);
	if (in.whole || (j->rtindex != 0 && is_read(q, j, j->rtindex, NULL)))
		return false;
	in.read = NULL;
	if (!raising_inline_ok(q, &in, j))
		return false;
	if (!apply)
		return true;
	if (j->jointype == JOIN_INNER)
	{
		List	   *conds = make_ands_implicit((Expr *) q->jointree->quals);

		if (j->quals != NULL)
			conds = lappend(conds, j->quals);
		if (gate != NULL)
		{
			gate = copyObject(gate);
			IncrementVarSublevelsUp(gate, -1, 1);
			conds = lappend(conds, gate);
		}
		q->jointree->quals = conds == NIL ? NULL : (Node *) make_ands_explicit(conds);
	}
	if (j->rtindex != 0)
		rt_fetch(j->rtindex, q->rtable)->jointype = JOIN_INNER;
	*at = j->larg;
	*q = *(Query *) query_tree_mutator(q, inline_mutator, &in, 0);
	return true;
}

bool
sift_agg_inline_laterals(Query *q, bool apply)
{
	bool		found = false;
	ListCell   *lc;

	if (q->commandType != CMD_SELECT || q->rowMarks != NIL || q->setOperations != NULL)
		return false;
	foreach(lc, q->jointree->fromlist)
		while (inline_in(q, (Node **) &lfirst(lc), apply))
		{
			if (!apply)
				return true;
			found = true;
		}
	return found;
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

/* `table.column = outer column` of one type, by the equality that groups the type, which has an ordering (the GROUP BY of the rewrite takes its sort operator) */
static bool
key_equality(Node *c, Var **in, Var **out)
{
	OpExpr	   *op = (OpExpr *) c;
	TypeCacheEntry *tc;
	Var		   *l,
			   *r;

	if (!IsA(c, OpExpr) || list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	*in = l->varlevelsup == 0 ? l : r;
	*out = l->varlevelsup == 0 ? r : l;
	if ((*in)->varlevelsup != 0 || (*in)->varno != 1 || (*in)->varattno <= 0 || (*out)->varlevelsup != 1 || (*out)->varattno <= 0 || (*in)->vartype != (*out)->vartype)
		return false;
	tc = lookup_type_cache((*in)->vartype, TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR);
	return tc->eq_opr == op->opno && OidIsValid(tc->lt_opr);
}

/* the query of one table as PostgreSQL 17 parses it, in place: no range table entry for its grouping (18 adds one for any GROUP BY), its columns the expressions they group by */
static Query *
ungrouped(Query *s)
{
#if PG_VERSION_NUM >= 180000
	if (s->hasGroupRTE && list_length(s->rtable) == 2 && llast_node(RangeTblEntry, s->rtable)->rtekind == RTE_GROUP)
	{
		s->targetList = (List *) ungroup_expr(s, (Node *) s->targetList);
		s->rtable = list_delete_last(s->rtable);
		s->hasGroupRTE = false;
	}
#endif
	return s;
}

/* a query of one table whose rows only its WHERE and GROUP BY pick, that cannot raise (the rewrite groups every row of the table, the sub-select only the rows of the keys asked) */
static bool
one_table(const Query *s)
{
	return s->commandType == CMD_SELECT && list_length(s->rtable) == 1 && rt_fetch(1, s->rtable)->rtekind == RTE_RELATION && list_length(s->jointree->fromlist) == 1 &&
		IsA(linitial(s->jointree->fromlist), RangeTblRef) && s->groupingSets == NIL && s->havingQual == NULL && !s->hasWindowFuncs && !s->hasTargetSRFs && !s->hasSubLinks &&
		s->distinctClause == NIL && s->limitCount == NULL && s->limitOffset == NULL && s->setOperations == NULL && s->cteList == NIL && s->rowMarks == NIL &&
		!contain_volatile_functions((Node *) s) && !agg_node_may_raise((Node *) s);
}

/* every column of the select list is an aggregate that is NULL over no row, as the column of a left join is for an outer row without a group */
static bool
null_on_empty_tlist(const Query *s)
{
	foreach_node(TargetEntry, te, s->targetList)
		if (!IsA(te->expr, Aggref) || !null_on_empty((Aggref *) te->expr))
			return false;
	return true;
}

/* the WHERE of s split into the equalities that tie it to the query a level up and the rest, which reads only its table */
static bool
split_ties(const Query *s, List **eqs, List **rest)
{
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

/* a sub-select of one aggregate, or an EXISTS, over one table tied to the query by equalities */
static bool
decor_subselect(const SubLink *sl, List **eqs, List **rest)
{
	Query	   *s = (Query *) sl->subselect;

	if (!one_table(s) || s->groupClause != NIL)
		return false;
	if (sl->subLinkType == EXISTS_SUBLINK ? s->hasAggs : sl->subLinkType != EXPR_SUBLINK || list_length(s->targetList) != 1 || !null_on_empty_tlist(s))
		return false;
	return split_ties(s, eqs, rest);
}

/* the sub-select of a LATERAL item of the query that is aggregates of one table tied to it by equalities: its groups, or one row of aggregates that are NULL over no row (a copy, ungrouped) */
static Query *
decor_lateral(const RangeTblEntry *rte, List **eqs, List **rest)
{
	Query	   *s;

	if (rte->rtekind != RTE_SUBQUERY || !rte->lateral)
		return NULL;
	s = ungrouped(copyObject(rte->subquery));
	if (!one_table(s) || !s->hasAggs || (s->groupClause == NIL && !null_on_empty_tlist(s)) || contain_vars_of_level((Node *) s->targetList, 1) || !split_ties(s, eqs, rest))
		return NULL;
	return s;
}

static RangeTblEntry *
join_entry(JoinType jointype)
{
	RangeTblEntry *j = makeNode(RangeTblEntry);

	j->rtekind = RTE_JOIN;
	j->jointype = jointype;
	j->eref = makeAlias("unnamed_join", NIL);
	j->inFromCl = true;
	return j;
}

/* the join of larg and rarg, a new range table entry */
static JoinExpr *
make_join(Query *q, JoinType jointype, Node *larg, Node *rarg)
{
	JoinExpr   *j = makeNode(JoinExpr);

	q->rtable = lappend(q->rtable, join_entry(jointype));
	j->jointype = jointype;
	j->larg = larg;
	j->rarg = rarg;
	j->rtindex = list_length(q->rtable);
	return j;
}

static RangeTblRef *
rt_ref(Index rti)
{
	RangeTblRef *r = makeNode(RangeTblRef);

	r->rtindex = rti;
	return r;
}

/*
 * g, of one table, without the ties in its WHERE and grouped by their columns as well (the last columns of its select list), as entry rti of the query; the equalities of the join to it.
 * The columns of its select list are the names of the entry.
 */
static List *
group_by_ties(Query *g, RangeTblEntry *rte, Index rti, List *eqs, List *rest)
{
	List	   *names = NIL,
			   *on = NIL;

	g->jointree->quals = (Node *) make_ands_explicit(rest);
	foreach_node(OpExpr, eq, eqs)
	{
		Var		   *in,
				   *out;
		SortGroupClause *sgc = makeNode(SortGroupClause);
		const int	resno = list_length(g->targetList) + 1;
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
		on = lappend(on, make_opclause(eq->opno, BOOLOID, false, (Expr *) makeVar(rti, resno, in->vartype, in->vartypmod, in->varcollid, 0), (Expr *) out, InvalidOid,
									   eq->inputcollid));
	}
	foreach_node(TargetEntry, te, g->targetList)
		names = lappend(names, makeString(te->resname != NULL ? te->resname : psprintf("c%d", te->resno)));
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = g;
	rte->lateral = false;
	rte->eref = makeAlias(rte->alias != NULL ? rte->alias->aliasname : "sift_decor", names);
	rte->inFromCl = true;
	return on;
}

typedef struct Decor
{
	Query	   *q;
	bool		apply;
	bool		changed;
} Decor;

/*
 * The sub-select grouped by the columns of its equalities, and the LEFT JOIN of the query to it on them, around the join tree of the query: for an aggregate its column, for an EXISTS
 * whether the outer row has a group (the key of a group is not NULL, the equality took no NULL)
 */
static Node *
decorrelate(Decor *d, SubLink *sl, List *eqs, List *rest)
{
	Query	   *q = d->q,
			   *g = (Query *) sl->subselect;
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	const Index rti = list_length(q->rtable) + 1;
	JoinExpr   *j;
	Var		   *col;
	NullTest   *found;

	if (sl->subLinkType == EXISTS_SUBLINK)
		g->targetList = NIL;
	rte->alias = makeAlias("sift_decor", NIL);
	q->rtable = lappend(q->rtable, rte);
	j = make_join(q, JOIN_LEFT, list_length(q->jointree->fromlist) == 1 ? linitial(q->jointree->fromlist) : (Node *) makeFromExpr(q->jointree->fromlist, NULL),
				  (Node *) rt_ref(rti));
	j->quals = (Node *) make_ands_explicit(group_by_ties(g, rte, rti, eqs, rest));
	q->jointree->fromlist = list_make1(j);
	col = makeVarFromTargetEntry(rti, linitial_node(TargetEntry, g->targetList));
	col->varnullingrels = bms_make_singleton(j->rtindex);
	d->changed = true;
	if (sl->subLinkType != EXISTS_SUBLINK)
		return (Node *) col;
	found = makeNode(NullTest);
	found->arg = (Expr *) col;
	found->nulltesttype = IS_NOT_NULL;
	found->location = -1;
	return (Node *) found;
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

/* a level of the statement whose rows are those of its FROM list: no aggregate, grouping, window, set-returning function or set operation */
static bool
flat_level(const Query *q)
{
	return !q->hasAggs && q->groupClause == NIL && q->groupingSets == NIL && !q->hasWindowFuncs && !q->hasTargetSRFs && q->setOperations == NULL && q->jointree->fromlist != NIL;
}

static bool
decorrelate_tlist(Query *q, bool apply)
{
	Decor		d = {q, apply, false};
	List	   *tlist;

	if (!q->hasSubLinks || !flat_level(q))
		return false;
	tlist = (List *) decor_mutator((Node *) q->targetList, &d);
	if (apply)
		q->targetList = tlist;
	return d.changed;
}

/* the join of the query whose right side is its last FROM item, `x, LATERAL (..)` (NULL: the comma form) or `x [LEFT] JOIN LATERAL (..) ON true`; 0: none */
static Index
lateral_item(const Query *q, JoinExpr **j)
{
	Node	   *last = llast(q->jointree->fromlist);

	*j = NULL;
	if (IsA(last, RangeTblRef))
		return list_length(q->jointree->fromlist) > 1 ? ((RangeTblRef *) last)->rtindex : 0;
	*j = (JoinExpr *) last;
	if (list_length(q->jointree->fromlist) != 1 || !IsA(last, JoinExpr) || ((*j)->jointype != JOIN_INNER && (*j)->jointype != JOIN_LEFT) || !IsA((*j)->rarg, RangeTblRef) ||
		((*j)->quals != NULL && !(IsA((*j)->quals, Const) && DatumGetBool(((Const *) (*j)->quals)->constvalue) && !((Const *) (*j)->quals)->constisnull)))
		return 0;
	return ((RangeTblRef *) (*j)->rarg)->rtindex;
}

/*
 * The lateral sub-select g tied to the query by eqs becomes g grouped by the tied columns as well, joined to the rest of the FROM list on the ties: an inner join keeps it for its groups; one row
 * of aggregates of every outer row is a left join, whose columns the conditions and the select list above it read as nulled by it
 */
static void
decorrelate_lateral(Query *q, Index lrti, JoinExpr *j, Query *g, List *eqs, List *rest)
{
	if (j == NULL)
	{
		j = make_join(q, JOIN_INNER, list_length(q->jointree->fromlist) == 2 ? linitial(q->jointree->fromlist) :
					  (Node *) makeFromExpr(list_delete_last(list_copy(q->jointree->fromlist)), NULL), (Node *) rt_ref(lrti));
		q->jointree->fromlist = list_make1(j);
	}
	if (g->groupClause == NIL && j->jointype != JOIN_LEFT)
	{
		const Bitmapset *l = bms_make_singleton(lrti),
				   *nulled = bms_make_singleton(j->rtindex);
		RangeTblEntry *jrte = rt_fetch(j->rtindex, q->rtable);

		q->targetList = (List *) add_nulling_relids((Node *) q->targetList, l, nulled);
		q->jointree->quals = add_nulling_relids(q->jointree->quals, l, nulled);
		jrte->joinaliasvars = (List *) add_nulling_relids((Node *) jrte->joinaliasvars, l, nulled);
		j->jointype = jrte->jointype = JOIN_LEFT;
	}
	j->quals = (Node *) make_ands_explicit(group_by_ties(g, rt_fetch(lrti, q->rtable), lrti, eqs, rest));
}

static bool
decorrelate_from(Query *q, bool apply)
{
	JoinExpr   *j;
	Index		lrti;
	List	   *eqs,
			   *rest;
	Query	   *g;

	if (!flat_level(q) || (lrti = lateral_item(q, &j)) == 0 || (g = decor_lateral(rt_fetch(lrti, q->rtable), &eqs, &rest)) == NULL)
		return false;
	if (apply)
		decorrelate_lateral(q, lrti, j, g, eqs, rest);
	return true;
}

bool
sift_agg_having_to_where(Query *q, bool apply)
{
	return q->commandType == CMD_SELECT && q->rowMarks == NIL && having_walker((Node *) q, &apply);
}

bool
sift_agg_flat_decorrelate(Query *q, bool apply)
{
	bool		from;

	if (q->commandType != CMD_SELECT || q->rowMarks != NIL)
		return false;
	from = decorrelate_from(q, apply);
	return decorrelate_tlist(q, apply) || from;
}
