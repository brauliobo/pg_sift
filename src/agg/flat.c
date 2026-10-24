/*
 * agg/flat.c - the lateral sub-selects PostgreSQL leaves in place, pulled up where the query level allows it.
 *
 * `a INNER JOIN LATERAL (SELECT ... FROM jsonb_each(a.doc) r LEFT JOIN m ON m.k = a.k AND m.x = r.key) s ON true` is the join of a with
 * the function and the table m: `(a JOIN jsonb_each(a.doc) r) LEFT JOIN m ON ...`. PostgreSQL does not pull that sub-select up when its
 * query ends up on the nullable side of an outer join of the enclosing query (`dates LEFT JOIN (that query)`: its columns would need the
 * outer join's nulling), and the scan sees a correlated child that needs the whole document. Inside its own query level the pull-up is
 * valid, so the planner hook does it there, plans the query both ways and keeps the flat one only if it runs in the scan and costs less.
 * A sub-select with an outer join is joined to the left side through its leftmost table, so that no outer join condition reads a table
 * outside the join: the shape PostgreSQL plans when it is written by hand.
 *
 * `x LEFT JOIN LATERAL (SELECT avg(e) FROM f(x.doc) WHERE ..) s ON true` has one row for each row of x: the aggregates of its elements are
 * a column of x, and the scan runs the grouping of the elements by the row of x (`.. FROM x LEFT JOIN LATERAL f(x.doc) GROUP BY x.id`). The
 * rewrite is made where that is the same query: x holds each of its keys once, and an aggregate of the elements the lateral left out is
 * what it is of the one row the outer join makes up (NULL for a strict argument).
 *
 * `x LEFT JOIN LATERAL (SELECT sum(f.a) FROM f WHERE f.k = x.k) s ON true` and the scalar sub-select `(SELECT sum(f.a) FROM f WHERE f.k = x.k)` are an
 * index lookup for each row of x. `f GROUP BY k` joined to x by k is the same rows when the sub-select is aggregates of one table tied to x by
 * equalities only (decor_*): the planner hook plans it too, and keeps it where it costs less than the lookups.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"

/* A sub-select PostgreSQL's own pull-up takes: no aggregate, window, SRF, grouping, ordering, limit, CTE, row mark or volatile function. */
static bool
simple_subquery(Query *q)
{
	return q->commandType == CMD_SELECT && q->setOperations == NULL && !q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs &&
		!q->hasSubLinks && !q->hasForUpdate && q->groupClause == NIL && q->groupingSets == NIL && q->havingQual == NULL &&
		q->sortClause == NIL && q->distinctClause == NIL && q->limitOffset == NULL && q->limitCount == NULL && q->cteList == NIL &&
		q->rowMarks == NIL && q->jointree != NULL && q->jointree->fromlist != NIL && !contain_volatile_functions((Node *) q);
}

static bool
true_or_missing(Node *quals)
{
	return quals == NULL || (IsA(quals, Const) && !((Const *) quals)->constisnull && DatumGetBool(((Const *) quals)->constvalue));
}

static bool
has_outer_join(Node *n)
{
	if (n == NULL)
		return false;
	if (IsA(n, FromExpr))
	{
		foreach_ptr(Node, x, ((FromExpr *) n)->fromlist)
			if (has_outer_join(x))
				return true;
	}
	else if (IsA(n, JoinExpr))
		return ((JoinExpr *) n)->jointype != JOIN_INNER || has_outer_join(((JoinExpr *) n)->larg) || has_outer_join(((JoinExpr *) n)->rarg);
	return false;
}

/* the sub-select's joins reach their leftmost table through inner and left joins, with no condition of their own on the way */
static bool
leftmost_spine(Node *n)
{
	if (IsA(n, FromExpr))
		return list_length(((FromExpr *) n)->fromlist) == 1 && ((FromExpr *) n)->quals == NULL && leftmost_spine(linitial(((FromExpr *) n)->fromlist));
	if (IsA(n, JoinExpr))
		return (((JoinExpr *) n)->jointype == JOIN_INNER || ((JoinExpr *) n)->jointype == JOIN_LEFT) && leftmost_spine(((JoinExpr *) n)->larg);
	return IsA(n, RangeTblRef);
}

/* `left INNER JOIN LATERAL (sub-select that reads left) ON true`: the sub-select's range table entry, or NULL */
static RangeTblEntry *
lateral_inner_child(Query *q, JoinExpr *j)
{
	RangeTblEntry *rte;

	if (j->jointype != JOIN_INNER || !IsA(j->rarg, RangeTblRef) || !true_or_missing(j->quals))
		return NULL;
	rte = rt_fetch(((RangeTblRef *) j->rarg)->rtindex, q->rtable);
	if (rte->rtekind != RTE_SUBQUERY || !rte->lateral || !simple_subquery(rte->subquery) || !contain_vars_of_level((Node *) rte->subquery, 1) ||
		(has_outer_join((Node *) rte->subquery->jointree) && !leftmost_spine((Node *) rte->subquery->jointree)))
		return NULL;
	return rte;
}

/* `left JOIN leftmost table` replaces the leftmost table of the sub-select's joins; the join keeps the lateral join's range table entry */
static Node *
join_leftmost(Query *q, Node *tree, Node *left, JoinExpr *lateral)
{
	JoinExpr   *j;
	RangeTblEntry *join_rte,
			   *leaf;
	List	   *names = NIL,
			   *vars = NIL;
	int			nleft;

	if (IsA(tree, FromExpr))
		return join_leftmost(q, linitial(((FromExpr *) tree)->fromlist), left, lateral);
	if (IsA(tree, JoinExpr))
	{
		((JoinExpr *) tree)->larg = join_leftmost(q, ((JoinExpr *) tree)->larg, left, lateral);
		return tree;
	}
	j = makeNode(JoinExpr);
	join_rte = rt_fetch(lateral->rtindex, q->rtable);
	leaf = rt_fetch(((RangeTblRef *) tree)->rtindex, q->rtable);
	nleft = list_length(join_rte->joinaliasvars) - list_length(rt_fetch(((RangeTblRef *) lateral->rarg)->rtindex, q->rtable)->eref->colnames);
	j->jointype = JOIN_INNER;
	j->larg = left;
	j->rarg = tree;
	j->rtindex = lateral->rtindex;
	/* the columns of the join: those of the left side, then those of the table */
#if PG_VERSION_NUM >= 180000
	expandRTE(leaf, ((RangeTblRef *) tree)->rtindex, 0, VAR_RETURNING_DEFAULT, -1, false, &names, &vars);
#else
	expandRTE(leaf, ((RangeTblRef *) tree)->rtindex, 0, -1, false, &names, &vars);
#endif
	join_rte->joinaliasvars = list_concat(list_copy_head(join_rte->joinaliasvars, nleft), vars);
	join_rte->eref = makeAlias(join_rte->eref->aliasname, list_concat(list_copy_head(join_rte->eref->colnames, nleft), names));
	return (Node *) j;
}

/* a pulled-up sub-select: the range table entry its columns are read through, and the expressions that compute them */
typedef struct Pulled
{
	Index		rti;
	RangeTblEntry *rte;
	List	   *targetlist;
} Pulled;

/*
 * The sub-select's range table, joins and conditions in the query's own, in place of the sub-select; its columns are replaced once the
 * joins are spliced (pulled). The node of the jointree that takes the join's place.
 */
static Node *
pull_up_lateral(Query *q, JoinExpr *j, RangeTblEntry *rte, List **pulled)
{
	Pulled	   *p;
	Query	   *sub;
	Node	   *node = (Node *) j;
	int			offset = list_length(q->rtable);

	p = palloc(sizeof(Pulled));
	sub = copyObject(rte->subquery);
	p->rti = ((RangeTblRef *) j->rarg)->rtindex;
	p->rte = rte;
	OffsetVarNodes((Node *) sub, offset, 0);
	IncrementVarSublevelsUp((Node *) sub, -1, 1);
	/* what was read from the enclosing query is now read from a table of the same level, and a function or sub-select may read it */
	foreach_node(RangeTblEntry, child, sub->rtable)
		if (child->rtekind == RTE_SUBQUERY || child->rtekind == RTE_FUNCTION || child->rtekind == RTE_VALUES || child->rtekind == RTE_TABLEFUNC)
			child->lateral = true;
	CombineRangeTables(&q->rtable, &q->rteperminfos, sub->rtable, sub->rteperminfos);
	if (has_outer_join((Node *) sub->jointree))
		node = join_leftmost(q, (Node *) sub->jointree, j->larg, j);
	else
		j->rarg = (Node *) sub->jointree;
	p->targetlist = sub->targetList;
	*pulled = lappend(*pulled, p);
	return node;
}

/* a copy of n that reads the expressions of the pulled-up sub-select in place of its columns */
static Node *
read_columns(Node *n, Pulled *p)
{
#if PG_VERSION_NUM >= 180000
	return ReplaceVarsFromTargetList(n, p->rti, 0, p->rte, p->targetlist, 0, REPLACEVARS_REPORT_ERROR, 0, NULL);
#else
	return ReplaceVarsFromTargetList(n, p->rti, 0, p->rte, p->targetlist, REPLACEVARS_REPORT_ERROR, 0, NULL);
#endif
}

static bool flatten_query(Query *q, bool apply);
static List *flat_levels;		/* the query level being flattened and those around it */
static bool group_aggregate_lateral(Query *q, bool apply);
struct DecorJoin;
static bool decorrelate_join(Query *q, JoinExpr *j, bool apply, List **decor);
static void decor_apply(Query *q, struct DecorJoin *r);
static bool decorrelate_sublinks(Query *q, bool apply);

/*
 * The joins that are not on the nullable side of an outer join of this level (a pulled-up column needs no nulling there). apply false only
 * tells whether there is a lateral sub-select to pull up. The node that takes the place of n.
 */
static Node *
flatten_jointree(Query *q, Node *n, bool apply, List **pulled, List **decor, bool *changed)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, FromExpr))
	{
		FromExpr   *f = (FromExpr *) n;
		List	   *items = NIL;

		foreach_ptr(Node, x, f->fromlist)
			items = lappend(items, flatten_jointree(q, x, apply, pulled, decor, changed));
		f->fromlist = items;
	}
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;
		RangeTblEntry *rte;

		if (j->jointype == JOIN_INNER || j->jointype == JOIN_LEFT)
			j->larg = flatten_jointree(q, j->larg, apply, pulled, decor, changed);
		if (j->jointype == JOIN_INNER)
			j->rarg = flatten_jointree(q, j->rarg, apply, pulled, decor, changed);
		if ((rte = lateral_inner_child(q, j)) != NULL)
		{
			*changed = true;
			/* the pulled-up joins may hold another one */
			return apply ? flatten_jointree(q, pull_up_lateral(q, j, rte, pulled), apply, pulled, decor, changed) : n;
		}
		if (decorrelate_join(q, j, apply, decor))
			*changed = true;
	}
	return n;
}

typedef struct SublinkFlat
{
	bool		apply;
	bool		changed;
} SublinkFlat;

/* the sub-selects of the expressions of a query level (its sublinks) are query levels of their own */
static bool
flatten_sublinks(Node *n, SublinkFlat *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		c->changed |= flatten_query((Query *) n, c->apply);
		return false;
	}
	return expression_tree_walker(n, flatten_sublinks, c);
}

static bool
flatten_query(Query *q, bool apply)
{
	bool		changed = false;
	List	   *pulled = NIL,
			   *decor = NIL;
	SublinkFlat sublinks = {apply, false};

	if (q->commandType != CMD_SELECT)
		return false;
	flat_levels = lcons(q, flat_levels);
	foreach_node(CommonTableExpr, cte, q->cteList)
		changed |= flatten_query(castNode(Query, cte->ctequery), apply);
	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind == RTE_SUBQUERY)
			changed |= flatten_query(rte->subquery, apply);
	query_tree_walker(q, flatten_sublinks, &sublinks, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
	changed |= sublinks.changed;
	q->jointree = (FromExpr *) flatten_jointree(q, (Node *) q->jointree, apply, &pulled, &decor, &changed);
	/* the copy ReplaceVarsFromTargetList makes takes the place of the query; a sub-select may read the columns of an earlier one */
	foreach_ptr(Pulled, p, pulled)
	{
		*q = *(Query *) read_columns((Node *) q, p);
		foreach_ptr(Pulled, later, pulled)
			later->targetlist = (List *) read_columns((Node *) later->targetlist, p);
	}
	foreach_ptr(struct DecorJoin, r, decor)
		decor_apply(q, r);
	changed |= decorrelate_sublinks(q, apply);
	changed |= group_aggregate_lateral(q, apply);
	changed |= sift_agg_latest_per_group(q, apply);
	flat_levels = list_delete_first(flat_levels);
	return changed;
}

/* a column of the lateral sub-select outside its aggregates, or an aggregate that is not NULL (or 0 for a count) of the row an outer join makes up */
static bool
unsafe_aggregate(Node *n, void *context)
{
	Aggref	   *a;
	char	   *name;
	Node	   *arg;

	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return true;
	if (!IsA(n, Aggref))
		return expression_tree_walker(n, unsafe_aggregate, context);
	a = (Aggref *) n;
	name = get_func_name(a->aggfnoid);
	if (a->agglevelsup != 0 || a->aggstar || a->aggfilter != NULL || a->aggorder != NIL || a->aggdistinct != NIL || a->aggkind != AGGKIND_NORMAL ||
		list_length(a->args) != 1 || name == NULL || get_func_namespace(a->aggfnoid) != PG_CATALOG_NAMESPACE ||
		(strcmp(name, "avg") != 0 && strcmp(name, "sum") != 0 && strcmp(name, "min") != 0 && strcmp(name, "max") != 0 && strcmp(name, "count") != 0))
		return true;
	arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
	return !contain_vars_of_level(arg, 0) || contain_nonstrict_functions(arg) || contain_volatile_functions(arg);
}

/* a sub-select of aggregates and constants over rows it reads from the query around it: exactly one row */
static bool
aggregate_lateral(Query *a)
{
	if (a->commandType != CMD_SELECT || !a->hasAggs || a->setOperations != NULL || a->hasWindowFuncs || a->hasTargetSRFs || a->hasSubLinks ||
		a->hasForUpdate || a->groupClause != NIL || a->groupingSets != NIL || a->havingQual != NULL || a->sortClause != NIL ||
		a->distinctClause != NIL || a->limitOffset != NULL || a->limitCount != NULL || a->cteList != NIL || a->rowMarks != NIL ||
		a->jointree == NULL || a->jointree->fromlist == NIL || contain_volatile_functions((Node *) a) || !contain_vars_of_level((Node *) a, 1))
		return false;
	foreach_node(TargetEntry, te, a->targetList)
		if (te->resjunk || unsafe_aggregate((Node *) te->expr, NULL))
			return false;
	return true;
}

/* the leaves and the conditions of a FROM tree of inner joins of tables, CTEs and sub-selects, or false */
static bool
inner_tables(Query *q, Node *n, List **tables, List **conds)
{
	if (n == NULL)
		return false;
	if (IsA(n, RangeTblRef))
	{
		RangeTblEntry *rte = rt_fetch(((RangeTblRef *) n)->rtindex, q->rtable);

		*tables = lappend_int(*tables, ((RangeTblRef *) n)->rtindex);
		return !rte->lateral && ((rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && rte->tablesample == NULL) ||
								 rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE);
	}
	if (IsA(n, FromExpr) || (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER))
	{
		Node	   *quals = IsA(n, FromExpr) ? ((FromExpr *) n)->quals : ((JoinExpr *) n)->quals;
		List	   *items = IsA(n, FromExpr) ? ((FromExpr *) n)->fromlist : list_make2(((JoinExpr *) n)->larg, ((JoinExpr *) n)->rarg);
		List	   *todo = quals == NULL ? NIL : list_make1(quals);

		foreach_ptr(Node, x, items)
			if (!inner_tables(q, x, tables, conds))
				return false;
		while (todo != NIL)
		{
			Node	   *c = linitial(todo);

			todo = list_delete_first(todo);
			if (IsA(c, BoolExpr) && ((BoolExpr *) c)->boolop == AND_EXPR)
				todo = list_concat_copy(((BoolExpr *) c)->args, todo);
			else
				*conds = lappend(*conds, c);
		}
		return true;
	}
	return false;
}

static List *unique_columns(Query *x, List *levels);

/* lcons that leaves the list alone */
static List *
below(Query *q, List *levels)
{
	return lcons(q, list_copy(levels));
}

/* the columns of a CTE or sub-select that hold its key once (levels: the query that reads it and those around it) */
static List *
leaf_key(RangeTblEntry *rte, List *levels)
{
	if (rte->rtekind == RTE_SUBQUERY)
		return unique_columns(rte->subquery, below(rte->subquery, levels));
	if (rte->rtekind == RTE_CTE && rte->ctelevelsup < list_length(levels))
		foreach_node(CommonTableExpr, cte, list_nth_node(Query, levels, rte->ctelevelsup)->cteList)
			if (strcmp(cte->ctename, rte->ctename) == 0)
				return unique_columns(castNode(Query, cte->ctequery), lcons(cte->ctequery, list_copy_tail(levels, rte->ctelevelsup)));
	return NIL;
}

/*
 * The columns of the output of x that hold one of its keys once: all of them for DISTINCT, else a unique key of a table (or of a CTE or sub-select
 * that has one) whose columns it shows, while the tables it joins to each have one row for a row of the others (their key is equal to a column
 * that is not theirs). NIL when there is none. levels: x and the queries around it.
 */
static List *
unique_columns(Query *x, List *levels)
{
	List	   *tables = NIL,
			   *conds = NIL;

	if (x->commandType != CMD_SELECT || x->setOperations != NULL || x->hasTargetSRFs || x->limitOffset != NULL || x->limitCount != NULL || x->rowMarks != NIL)
		return NIL;
	if (x->distinctClause != NIL && !x->hasDistinctOn)
	{
		List	   *all = NIL;

		foreach_node(TargetEntry, te, x->targetList)
			if (!te->resjunk)
				all = lappend_int(all, te->resno);
		return all;
	}
	if (x->hasAggs || x->hasWindowFuncs || x->groupClause != NIL || x->groupingSets != NIL || x->havingQual != NULL || x->distinctClause != NIL ||
		x->jointree == NULL || !inner_tables(x, (Node *) x->jointree, &tables, &conds))
		return NIL;
	foreach_int(rti, tables)
	{
		RangeTblEntry *rte = rt_fetch(rti, x->rtable);
		int			shown[16];
		int			nshown = 0;
		bool		others = true;
		bool		shown_key = true;
		List	   *key = NIL,
				   *columns = NIL;

		foreach_node(TargetEntry, te, x->targetList)
		{
			Var		   *v = (Var *) te->expr;

			if (IsA(v, Var) && v->varno == rti && v->varlevelsup == 0 && nshown < lengthof(shown))
				shown[nshown++] = v->varattno;
		}
		if (rte->rtekind == RTE_RELATION)
		{
			if (nshown == 0 || !sift_agg_unique_key(rte->relid, 0, nshown, shown, &key))
				continue;
		}
		else if ((key = leaf_key(rte, levels)) == NIL)
			continue;
		/* the key is shown */
		foreach_int(att, key)
		{
			bool		is_shown = false;

			foreach_node(TargetEntry, te, x->targetList)
				is_shown |= IsA(te->expr, Var) && ((Var *) te->expr)->varno == rti && ((Var *) te->expr)->varattno == att;
			shown_key &= is_shown;
		}
		if (!shown_key)
			continue;
		foreach_int(other, tables)
		{
			RangeTblEntry *o = rt_fetch(other, x->rtable);
			int			bound[16];
			int			nbound = 0;
			bool		unique;

			if (other == rti)
				continue;
			foreach_ptr(Node, c, conds)
			{
				OpExpr	   *op = (OpExpr *) c;

				if (!IsA(op, OpExpr) || list_length(op->args) != 2 || !op_mergejoinable(op->opno, exprType(linitial(op->args))))
					continue;
				for (int side = 0; side < 2; side++)
				{
					Var		   *v = (Var *) list_nth(op->args, side);

					if (IsA(v, Var) && v->varno == other && v->varlevelsup == 0 && nbound < lengthof(bound) &&
						!bms_is_member(other, pull_varnos(NULL, list_nth(op->args, 1 - side))))
						bound[nbound++] = v->varattno;
				}
			}
			if (o->rtekind == RTE_RELATION)
				unique = sift_agg_unique_key(o->relid, nbound, 0, bound, NULL);
			else
			{
				List	   *okey = leaf_key(o, levels);

				unique = okey != NIL;
				foreach_int(att, okey)
				{
					bool		is_bound = false;

					for (int i = 0; i < nbound; i++)
						is_bound |= bound[i] == att;
					unique &= is_bound;
				}
			}
			if (!unique)
			{
				others = false;
				break;
			}
		}
		if (!others)
			continue;
		foreach_node(TargetEntry, te, x->targetList)
			if (IsA(te->expr, Var) && ((Var *) te->expr)->varno == rti && list_member_int(key, ((Var *) te->expr)->varattno))
				columns = list_append_unique_int(columns, te->resno);
		return columns;
	}
	return NIL;
}

static void
group_by(Query *q, TargetEntry *te)
{
	SortGroupClause *g = makeNode(SortGroupClause);
	Oid			lt,
				eq,
				gt;
	Index		ref = 0;

	foreach_node(TargetEntry, other, q->targetList)
		ref = Max(ref, other->ressortgroupref);
	if (te->ressortgroupref == 0)
		te->ressortgroupref = ref + 1;
	get_sort_group_operators(exprType((Node *) te->expr), false, true, false, &lt, &eq, &gt, &g->hashable);
	g->tleSortGroupRef = te->ressortgroupref;
	g->eqop = eq;
	g->sortop = lt;
	q->groupClause = lappend(q->groupClause, g);
}

/* max(e), or NULL where the type has none */
static Expr *
max_of(Expr *e)
{
	Oid			type = exprType((Node *) e);
	Oid			max = LookupFuncName(list_make1(makeString("max")), 1, &type, true);
	Aggref	   *a;

	if (!OidIsValid(max) || get_func_prokind(max) != PROKIND_AGGREGATE)
		return NULL;
	a = makeNode(Aggref);
	a->aggfnoid = max;
	a->aggtype = type;
	a->aggcollid = exprCollation((Node *) e);
	a->inputcollid = a->aggcollid;
	a->aggargtypes = list_make1_oid(type);
	a->args = list_make1(makeTargetEntry(e, 1, NULL, false));
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	return (Expr *) a;
}

/*
 * `SELECT .. FROM x LEFT JOIN LATERAL (SELECT aggregates FROM f(x..) WHERE ..) s ON true` as the one level of the query, with the grouping by the
 * key of x that the aggregates of the elements had in their own. The columns of x that are not keys are grouped too, or their max.
 */
static bool
group_aggregate_lateral(Query *q, bool apply)
{
	FromExpr   *f = q->jointree;
	JoinExpr   *j;
	RangeTblEntry *x,
			   *lateral;
	Index		xrti,
				lrti;
	List	   *keys = NIL;
	Pulled		p;
	int			offset;
	Query	   *sub;
	List	   *tlist;

	if (q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->setOperations != NULL ||
		q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL || q->distinctClause != NIL || q->sortClause != NIL ||
		q->limitOffset != NULL || q->limitCount != NULL || q->cteList != NIL || q->rowMarks != NIL || f == NULL || f->quals != NULL ||
		list_length(f->fromlist) != 1 || !IsA(linitial(f->fromlist), JoinExpr))
		return false;
	j = linitial_node(JoinExpr, f->fromlist);
	if (j->jointype != JOIN_LEFT || !true_or_missing(j->quals) || !IsA(j->larg, RangeTblRef) || !IsA(j->rarg, RangeTblRef))
		return false;
	xrti = ((RangeTblRef *) j->larg)->rtindex;
	lrti = ((RangeTblRef *) j->rarg)->rtindex;
	x = rt_fetch(xrti, q->rtable);
	lateral = rt_fetch(lrti, q->rtable);
	if (x->lateral || lateral->rtekind != RTE_SUBQUERY || !lateral->lateral || !aggregate_lateral(lateral->subquery) ||
		(x->rtekind != RTE_SUBQUERY && (x->rtekind != RTE_RELATION || x->relkind != RELKIND_RELATION || x->tablesample != NULL)))
		return false;
	/* a column of the lateral is read alone: an expression of it and of x would be a column of the group that is neither grouped nor aggregated */
	foreach_node(TargetEntry, te, q->targetList)
		if (bms_is_member(lrti, pull_varnos(NULL, (Node *) te->expr)) && !IsA(te->expr, Var))
			return false;
	/* the key of x, as the numbers of its columns */
	if (x->rtekind == RTE_RELATION)
	{
		int		   *all = palloc_array(int, list_length(x->eref->colnames));

		for (int i = 0; i < list_length(x->eref->colnames); i++)
			all[i] = i + 1;
		if (!sift_agg_unique_key(x->relid, 0, list_length(x->eref->colnames), all, &keys))
			return false;
	}
	else if ((keys = unique_columns(x->subquery, below(x->subquery, flat_levels))) == NIL)
		return false;
	if (!apply)
		return true;

	p.rti = lrti;
	p.rte = lateral;
	sub = copyObject(lateral->subquery);
	offset = list_length(q->rtable);
	OffsetVarNodes((Node *) sub, offset, 0);
	IncrementVarSublevelsUp((Node *) sub, -1, 1);
	foreach_node(RangeTblEntry, child, sub->rtable)
		if (child->rtekind == RTE_SUBQUERY || child->rtekind == RTE_FUNCTION || child->rtekind == RTE_VALUES || child->rtekind == RTE_TABLEFUNC)
			child->lateral = true;
	CombineRangeTables(&q->rtable, &q->rteperminfos, sub->rtable, sub->rteperminfos);
	j->rarg = (Node *) sub->jointree;
	p.targetlist = sub->targetList;
	*q = *(Query *) read_columns((Node *) q, &p);
	q->hasAggs = true;
	/* what reads x: grouped (its columns), or the max of an expression of them */
	tlist = q->targetList;
	foreach_node(TargetEntry, te, tlist)
	{
		Expr	   *m;

		if (contain_agg_clause((Node *) te->expr) || !contain_vars_of_level((Node *) te->expr, 0))
			continue;
		if (IsA(te->expr, Var) || (m = max_of(te->expr)) == NULL)
		{
			if (te->ressortgroupref == 0)
				group_by(q, te);
		}
		else
			te->expr = m;
	}
	foreach_int(att, keys)
	{
		Var		   *v;
		Oid			type,
					coll;
		int32		typmod;
		bool		shown = false;

		if (x->rtekind == RTE_SUBQUERY)
			v = makeVarFromTargetEntry(xrti, list_nth_node(TargetEntry, x->subquery->targetList, att - 1));
		else
		{
			get_atttypetypmodcoll(x->relid, att, &type, &typmod, &coll);
			v = makeVar(xrti, att, type, typmod, coll, 0);
		}
		foreach_node(TargetEntry, te, q->targetList)
			if (equal(te->expr, v) && te->ressortgroupref != 0)
				shown = true;
		if (!shown)
		{
			TargetEntry *te = makeTargetEntry((Expr *) v, list_length(q->targetList) + 1, NULL, true);

			q->targetList = lappend(q->targetList, te);
			group_by(q, te);
		}
	}
	return true;
}

/*
 * A sub-select of aggregates over one table, tied to the query around it by equalities of columns (`SELECT sum(f.a) FROM f WHERE f.k = x.k AND ..`, a LATERAL
 * sub-select or a scalar sublink), is `f GROUP BY k` joined to x by k. The rows of x do not change: the groups hold each k once, a NULL k matches no
 * group (as no row of f equals it) and a count of the rows a k does not have is 0 (every other aggregate is NULL). The planner hook plans the query both ways
 * and keeps this one if it runs in the scan and costs less: an index lookup for each of a few rows of x stays what it was.
 */
typedef struct Decor
{
	Query	   *sub;
	List	   *conds;			/* the equalities: the table's column and the outer query's */
	List	   *rest;			/* the other conditions of the sub-select */
	Bitmapset  *outer;			/* the range table entries of the outer query it reads */
} Decor;

/* the aggregates whose value over no rows is NULL, or 0 (count) */
static bool
decor_aggregate(Aggref *a)
{
	char	   *name = get_func_name(a->aggfnoid);

	return a->agglevelsup == 0 && a->aggkind == AGGKIND_NORMAL && name != NULL && get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE &&
		(strcmp(name, "count") == 0 || strcmp(name, "sum") == 0 || strcmp(name, "avg") == 0 || strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
		 strcmp(name, "array_agg") == 0 || strcmp(name, "bool_and") == 0 || strcmp(name, "bool_or") == 0 || strcmp(name, "string_agg") == 0) &&
		!contain_vars_of_level((Node *) a, 1);
}

/* the expressions of the select list: aggregates, constants and functions of them, no column outside an aggregate */
static bool
decor_output_bad(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return true;
	if (IsA(n, Aggref))
		return !decor_aggregate((Aggref *) n);
	return expression_tree_walker(n, decor_output_bad, context);
}

/* `table.column = outer column` of the same type, by the equality that groups the type */
static bool
decor_equality(Node *c, Decor *d)
{
	OpExpr	   *op;
	Var		   *l,
			   *r,
			   *in,
			   *out;
	TypeCacheEntry *t;

	if (!IsA(c, OpExpr) || list_length((op = (OpExpr *) c)->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	in = l->varlevelsup == 0 ? l : r;
	out = l->varlevelsup == 0 ? r : l;
	if (in->varlevelsup != 0 || in->varno != 1 || in->varattno < 1 || out->varlevelsup != 1 || out->varattno < 1 || in->vartype != out->vartype)
		return false;
	t = lookup_type_cache(in->vartype, TYPECACHE_EQ_OPR);
	if (!OidIsValid(t->eq_opr) || t->eq_opr != op->opno)
		return false;
	d->outer = bms_add_member(d->outer, out->varno);
	return true;
}

static bool
decor_analyze(Query *sub, Decor *d)
{
	RangeTblEntry *t;

	if (sub->commandType != CMD_SELECT || !sub->hasAggs || sub->hasSubLinks || sub->hasWindowFuncs || sub->hasTargetSRFs || sub->hasForUpdate ||
		sub->setOperations != NULL || sub->groupClause != NIL || sub->groupingSets != NIL || sub->havingQual != NULL || sub->sortClause != NIL ||
		sub->distinctClause != NIL || sub->limitOffset != NULL || sub->limitCount != NULL || sub->cteList != NIL || sub->rowMarks != NIL ||
		sub->jointree == NULL || list_length(sub->jointree->fromlist) != 1 || !IsA(linitial(sub->jointree->fromlist), RangeTblRef) ||
		list_length(sub->rtable) != 1 || contain_volatile_functions((Node *) sub) || contain_vars_of_level((Node *) sub, 2))
		return false;
	t = linitial_node(RangeTblEntry, sub->rtable);
	if (t->rtekind != RTE_RELATION || t->relkind != RELKIND_RELATION || t->tablesample != NULL || !has_column_index(t->relid))
		return false;
	d->sub = sub;
	foreach_ptr(Node, c, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		if (!contain_vars_of_level(c, 1))
			d->rest = lappend(d->rest, c);
		else if (decor_equality(c, d))
			d->conds = lappend(d->conds, c);
		else
			return false;
	}
	foreach_node(TargetEntry, te, sub->targetList)
		if (te->resjunk || decor_output_bad((Node *) te->expr, NULL))
			return false;
	return d->conds != NIL;
}

typedef struct DecorOut
{
	Query	   *g;				/* the grouped query being built */
	Index		grti;			/* its range table entry in the outer query */
	Bitmapset  *nulled;			/* the join that makes its columns NULL for an outer row without a group (a sublink: the new join; a lateral: its own) */
} DecorOut;

/* an aggregate becomes a column of the grouped query (a count of no rows is 0, not NULL) */
static Node *
decor_column(Node *n, DecorOut *o)
{
	Aggref	   *a;
	Var		   *v;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Aggref))
		return expression_tree_mutator(n, decor_column, o);
	a = (Aggref *) n;
	o->g->targetList = lappend(o->g->targetList, makeTargetEntry((Expr *) copyObject(a), list_length(o->g->targetList) + 1, psprintf("a%d", list_length(o->g->targetList) + 1), false));
	v = makeVar(o->grti, list_length(o->g->targetList), a->aggtype, -1, a->aggcollid, 0);
	v->varnullingrels = o->nulled;
	if (a->aggfnoid == F_COUNT_ANY || a->aggfnoid == F_COUNT_)
	{
		CoalesceExpr *c = makeNode(CoalesceExpr);

		c->coalescetype = INT8OID;
		c->args = list_make2(v, makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, FLOAT8PASSBYVAL));
		c->location = -1;
		return (Node *) c;
	}
	return (Node *) v;
}

/*
 * The sub-select as the table grouped by the columns the outer query is tied to. exprs: what each column of the sub-select is, in terms of the columns of
 * the grouped query (the new range table entry grti, whose columns the join nulls: nulled); join: the equalities between the outer query and the grouped one.
 */
static Query *
decor_group(Decor *d, Index grti, Bitmapset *nulled, List **exprs, List **join)
{
	Query	   *g = copyObject(d->sub);
	List	   *keys = NIL;
	DecorOut	o = {g, grti, nulled};

	g->jointree->quals = d->rest == NIL ? NULL : (Node *) make_ands_explicit(d->rest);
	g->targetList = NIL;
	g->hasAggs = true;
	foreach_ptr(OpExpr, c, d->conds)
	{
		bool		first = castNode(Var, linitial(c->args))->varlevelsup == 0;
		Var		   *in = castNode(Var, first ? linitial(c->args) : lsecond(c->args));
		OpExpr	   *e = copyObject(c);
		int			at = 0;

		foreach_int(att, keys)
			if (att == in->varattno)
				at = foreach_current_index(att) + 1;
		if (at == 0)
		{
			TargetEntry *te = makeTargetEntry((Expr *) copyObject(in), list_length(g->targetList) + 1, psprintf("k%d", list_length(g->targetList) + 1), false);

			g->targetList = lappend(g->targetList, te);
			group_by(g, te);
			keys = lappend_int(keys, in->varattno);
			at = list_length(keys);
		}
		/* the keys are the first columns of the grouped query, in the order the equalities name them */
		castNode(Var, first ? lsecond(e->args) : linitial(e->args))->varlevelsup = 0;
		lfirst(list_nth_cell(e->args, first ? 0 : 1)) = makeVar(grti, at, in->vartype, in->vartypmod, in->varcollid, 0);
		*join = lappend(*join, e);
	}
	foreach_node(TargetEntry, te, d->sub->targetList)
		*exprs = lappend(*exprs, makeTargetEntry((Expr *) decor_column((Node *) te->expr, &o), te->resno, te->resname, false));
	return g;
}

/* the range table entries a FROM tree holds, joins included */
static Bitmapset *
tree_rtis(Node *n)
{
	Bitmapset  *s = NULL;

	if (IsA(n, RangeTblRef))
		return bms_make_singleton(((RangeTblRef *) n)->rtindex);
	if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;

		s = bms_union(tree_rtis(j->larg), tree_rtis(j->rarg));
		if (j->rtindex != 0)
			s = bms_add_member(s, j->rtindex);
	}
	return s;
}

/* a lateral sub-select to replace (the replacement waits for the joins of the level to be spliced, as for a pull-up) */
typedef struct DecorJoin
{
	Index		lrti;
	Decor		d;
} DecorJoin;

static bool decorrelated;		/* a rewrite of this kind was made: the plan it gives is chosen by cost */

/* `x [LEFT | INNER] JOIN LATERAL (aggregates of a table tied to x) ON true` */
static bool
decorrelate_join(Query *q, JoinExpr *j, bool apply, List **decor)
{
	RangeTblEntry *rte;
	DecorJoin  *r = palloc0(sizeof(DecorJoin));

	if ((j->jointype != JOIN_LEFT && j->jointype != JOIN_INNER) || !IsA(j->rarg, RangeTblRef) || !true_or_missing(j->quals))
		return false;
	r->lrti = ((RangeTblRef *) j->rarg)->rtindex;
	rte = rt_fetch(r->lrti, q->rtable);
	if (rte->rtekind != RTE_SUBQUERY || !rte->lateral || !decor_analyze(rte->subquery, &r->d) || !bms_is_subset(r->d.outer, tree_rtis(j->larg)))
		return false;
	if (apply)
		*decor = lappend(*decor, r);
	return true;
}

static JoinExpr *
join_of(Node *n, Index rti)
{
	if (IsA(n, FromExpr))
	{
		foreach_ptr(Node, x, ((FromExpr *) n)->fromlist)
		{
			JoinExpr   *j = join_of(x, rti);

			if (j != NULL)
				return j;
		}
	}
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;

		if (IsA(j->rarg, RangeTblRef) && ((RangeTblRef *) j->rarg)->rtindex == rti)
			return j;
		return join_of(j->larg, rti) ? join_of(j->larg, rti) : join_of(j->rarg, rti);
	}
	return NULL;
}

typedef struct DecorRead
{
	List	   *exprs;			/* the columns of the lateral sub-select, as expressions of the grouped columns */
	Bitmapset  *nulled;			/* the join, which now makes them NULL for a row without a group (an inner join did not) */
} DecorRead;

/* a column of the lateral sub-select read where the join nulls it */
static Node *
decor_read(Var *var, replace_rte_variables_context *context)
{
	DecorRead  *read = context->callback_arg;
	List	   *exprs = read->exprs;
	Node	   *e;

	if (var->varattno == 0)
	{
		RowExpr    *row = makeNode(RowExpr);

		row->row_typeid = RECORDOID;
		row->row_format = COERCE_IMPLICIT_CAST;
		row->location = -1;
		foreach_node(TargetEntry, te, exprs)
		{
			row->args = lappend(row->args, copyObject(te->expr));
			row->colnames = lappend(row->colnames, makeString(te->resname));
		}
		e = (Node *) row;
	}
	else
		e = (Node *) copyObject(list_nth_node(TargetEntry, exprs, var->varattno - 1)->expr);
	if (var->varlevelsup > 0)
		IncrementVarSublevelsUp(e, var->varlevelsup, 0);
	return add_nulling_relids(e, NULL, bms_union(var->varnullingrels, read->nulled));
}

/* the sub-select becomes the table grouped, the join a left join on the equalities, its columns the expressions of the grouped columns */
static void
decor_apply(Query *q, DecorJoin *r)
{
	List	   *exprs = NIL,
			   *join = NIL,
			   *names = NIL;
	Query	   *g = decor_group(&r->d, r->lrti, NULL, &exprs, &join);
	RangeTblEntry *rte;
	JoinExpr   *j;
	DecorRead	read;

	j = join_of((Node *) q->jointree, r->lrti);
	read.exprs = exprs;
	read.nulled = bms_make_singleton(j->rtindex);
	*q = *(Query *) replace_rte_variables((Node *) q, r->lrti, 0, decor_read, &read, NULL);
	rte = rt_fetch(r->lrti, q->rtable);
	rte->subquery = g;
	rte->lateral = false;
	foreach_node(TargetEntry, te, g->targetList)
		names = lappend(names, makeString(te->resname));
	rte->eref = makeAlias(rte->eref->aliasname, names);
	j = join_of((Node *) q->jointree, r->lrti);
	j->jointype = JOIN_LEFT;
	rt_fetch(j->rtindex, q->rtable)->jointype = JOIN_LEFT;
	j->quals = (Node *) make_ands_explicit(join);
	decorrelated = true;
}

typedef struct SublinkDecor
{
	Query	   *q;
	bool		apply;
	bool		changed;
	List	   *pulled;
} SublinkDecor;

/* the columns of a FROM item, as a join's range table entry holds them */
static void
join_columns(Query *q, Node *n, List **names, List **vars)
{
	Index		rti = IsA(n, RangeTblRef) ? ((RangeTblRef *) n)->rtindex : ((JoinExpr *) n)->rtindex;
	List	   *nn,
			   *vv;

#if PG_VERSION_NUM >= 180000
	expandRTE(rt_fetch(rti, q->rtable), rti, 0, VAR_RETURNING_DEFAULT, -1, false, &nn, &vv);
#else
	expandRTE(rt_fetch(rti, q->rtable), rti, 0, -1, false, &nn, &vv);
#endif
	*names = list_concat(*names, nn);
	*vars = list_concat(*vars, vv);
}

/* `(SELECT aggregate FROM t WHERE t.k = x.k)` of a select list or WHERE: the item of the FROM list that holds x LEFT JOIN the table grouped */
static Node *
decor_sublink(Node *n, SublinkDecor *c)
{
	SubLink    *s;
	Decor		d = {0};
	Query	   *q = c->q;
	List	   *exprs = NIL,
			   *join = NIL,
			   *names = NIL,
			   *vars = NIL;
	ListCell   *item = NULL;
	RangeTblEntry *g,
			   *jr;
	JoinExpr   *j;
	Index		grti;
	int			nleft;

	if (n == NULL)
		return NULL;
	if (!IsA(n, SubLink))
		return expression_tree_mutator(n, decor_sublink, c);
	s = (SubLink *) n;
	if (s->subLinkType != EXPR_SUBLINK || !decor_analyze(castNode(Query, s->subselect), &d))
		return n;
	foreach(item, q->jointree->fromlist)
		if (bms_is_subset(d.outer, tree_rtis(lfirst(item))))
			break;
	if (item == NULL)
		return n;
	c->changed = true;
	if (!c->apply)
		return n;
	grti = list_length(q->rtable) + 1;
	g = makeNode(RangeTblEntry);
	g->rtekind = RTE_SUBQUERY;
	g->subquery = decor_group(&d, grti, bms_make_singleton(grti + 1), &exprs, &join);
	decorrelated = true;
	g->inFromCl = true;
	foreach_node(TargetEntry, te, g->subquery->targetList)
		names = lappend(names, makeString(te->resname));
	g->eref = makeAlias("decor", names);
	q->rtable = lappend(q->rtable, g);

	/* x LEFT JOIN g ON equalities, with the range table entry of the join that the planner expects */
	names = vars = NIL;
	join_columns(q, lfirst(item), &names, &vars);
	nleft = list_length(names);
	{
		RangeTblRef *ref = makeNode(RangeTblRef);

		ref->rtindex = grti;
		j = makeNode(JoinExpr);
		j->jointype = JOIN_LEFT;
		j->larg = lfirst(item);
		j->rarg = (Node *) ref;
		j->quals = (Node *) make_ands_explicit(join);
		join_columns(q, (Node *) ref, &names, &vars);
	}
	jr = makeNode(RangeTblEntry);
	jr->rtekind = RTE_JOIN;
	jr->jointype = JOIN_LEFT;
	jr->joinaliasvars = vars;
	for (int i = 1; i <= list_length(vars); i++)
		if (i <= nleft)
			jr->joinleftcols = lappend_int(jr->joinleftcols, i);
		else
			jr->joinrightcols = lappend_int(jr->joinrightcols, i - nleft);
	jr->eref = makeAlias("unnamed_join", names);
	jr->inFromCl = true;
	q->rtable = lappend(q->rtable, jr);
	j->rtindex = list_length(q->rtable);
	lfirst(item) = j;
	return (Node *) linitial_node(TargetEntry, exprs)->expr;
}

static bool
decorrelate_sublinks(Query *q, bool apply)
{
	SublinkDecor c = {q, apply, false, NIL};

	if (!q->hasSubLinks || q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->groupClause != NIL || q->groupingSets != NIL ||
		q->havingQual != NULL || q->jointree == NULL || q->setOperations != NULL)
		return false;
	foreach_node(TargetEntry, te, q->targetList)
		te->expr = (Expr *) decor_sublink((Node *) te->expr, &c);
	q->jointree->quals = decor_sublink(q->jointree->quals, &c);
	return c.changed;
}

bool
sift_agg_has_flattenable_laterals(Query *q)
{
	flat_levels = NIL;
	return flatten_query(q, false);
}

bool
sift_agg_flatten_laterals(Query *q)
{
	flat_levels = NIL;
	decorrelated = false;
	flatten_query(q, true);
	return decorrelated;
}
