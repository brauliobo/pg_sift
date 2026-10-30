/*
 * agg/flat/decor.c - the aggregates of one table tied to the outer query by equalities
 * The sub-select or sublink of aggregates over one table, rewritten as that table grouped by
 * the tying columns and joined to the outer query.
 */

/*
 * `x LEFT JOIN LATERAL (SELECT sum(f.a) FROM f WHERE f.k = x.k) s ON true` and the scalar sub-select `(SELECT sum(f.a) FROM f WHERE f.k = x.k)` are an
 * index lookup for each row of x. `f GROUP BY k` joined to x by k is the same rows when the sub-select is aggregates of one table tied to x by
 * equalities only (decor_*): the planner hook plans it too, and keeps it where it costs less than the lookups.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

#if PG_VERSION_NUM >= 180000
typedef struct Ungroup
{
	Index		rti;
	List	   *exprs;
} Ungroup;

/* a column of the entry that stands for the grouping is the expression it groups by */
static Node *
ungroup_column(Node *n, Ungroup *u)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varno == u->rti)
		return copyObject(list_nth(u->exprs, ((Var *) n)->varattno - 1));
	return expression_tree_mutator(n, ungroup_column, u);
}
#endif

typedef struct DecorOut
{
	Query	   *g;				/* the grouped query being built */
	Index		grti;			/* its range table entry in the outer query */
	Bitmapset  *nulled;			/* the join that makes its columns NULL for an outer row without a group (a sublink: the new join; a lateral: its own) */
} DecorOut;

typedef struct DecorRead
{
	List	   *exprs;			/* the columns of the lateral sub-select, as expressions of the grouped columns */
	Bitmapset  *nulled;			/* the join, which now makes them NULL for a row without a group (an inner join did not) */
} DecorRead;

typedef struct SublinkDecor
{
	Query	   *q;
	bool		apply;
	bool		changed;
	List	   *semi;			/* the EXISTS and NOT EXISTS the WHERE ANDs: PostgreSQL makes semi and anti joins of them, which the scan has arms for */
} SublinkDecor;

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

/* the expressions of the select list: aggregates, the expressions it groups by, constants and functions of them, no column outside an aggregate */
static bool
decor_output_bad(Node *n, Decor *d)
{
	if (n == NULL)
		return false;
	foreach_ptr(Node, g, d->groups)
		if (equal(n, g))
			return false;
	if (IsA(n, Var))
		return true;
	if (IsA(n, Aggref))
		return !decor_aggregate((Aggref *) n);
	return expression_tree_walker(n, decor_output_bad, d);
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

/* the sub-select as PostgreSQL 17 parses it: no range table entry for its grouping (PostgreSQL 18 adds one for any GROUP BY) */
static Query *
ungrouped(Query *sub)
{
#if PG_VERSION_NUM >= 180000
	Query	   *q;
	Ungroup		u;

	if (!sub->hasGroupRTE)
		return sub;
	q = copyObject(sub);
	u.rti = list_length(q->rtable);
	u.exprs = llast_node(RangeTblEntry, q->rtable)->groupexprs;
	q->targetList = (List *) ungroup_column((Node *) q->targetList, &u);
	q->havingQual = ungroup_column(q->havingQual, &u);
	q->rtable = list_delete_last(q->rtable);
	q->hasGroupRTE = false;
	return q;
#else
	return sub;
#endif
}

static bool
decor_analyze(Query *sub, Decor *d)
{
	RangeTblEntry *t;

	sub = ungrouped(sub);
	if (sub->commandType != CMD_SELECT || (sub->hasAggs || sub->groupClause != NIL) != !d->exists || sub->hasSubLinks || sub->hasWindowFuncs || sub->hasTargetSRFs || sub->hasForUpdate ||
		sub->setOperations != NULL || sub->groupingSets != NIL || sub->groupDistinct || (sub->groupClause == NIL && sub->havingQual != NULL) || sub->sortClause != NIL ||
		sub->distinctClause != NIL || sub->limitOffset != NULL || sub->limitCount != NULL || sub->cteList != NIL || sub->rowMarks != NIL ||
		sub->jointree == NULL || list_length(sub->jointree->fromlist) != 1 || !IsA(linitial(sub->jointree->fromlist), RangeTblRef) ||
		list_length(sub->rtable) != 1 || contain_volatile_functions((Node *) sub) || contain_vars_of_level((Node *) sub, 2))
		return false;
	t = linitial_node(RangeTblEntry, sub->rtable);
	if (t->rtekind != RTE_RELATION || t->relkind != RELKIND_RELATION || t->tablesample != NULL || !has_column_source(t->relid))
		return false;
	d->sub = sub;
	if (contain_vars_of_level(sub->havingQual, 1))
		return false;
	foreach_ptr(SortGroupClause, sgc, sub->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, sub->targetList);

		if (contain_vars_of_level(e, 1))
			return false;
		d->groups = lappend(d->groups, e);
	}
	foreach_ptr(Node, c, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		if (!contain_vars_of_level(c, 1))
			d->rest = lappend(d->rest, c);
		else if (decor_equality(c, d))
			d->conds = lappend(d->conds, c);
		else
			return false;
	}
	if (!d->exists)
		foreach_node(TargetEntry, te, sub->targetList)
			if ((te->resjunk && te->ressortgroupref == 0) || decor_output_bad((Node *) te->expr, d))
				return false;
	return d->conds != NIL;
}

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
	g->groupClause = NIL;
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
	/* then the groups of the sub-select (the grouped column of a tie is that key); the HAVING stays in the grouped query */
	foreach_ptr(SortGroupClause, sgc, d->sub->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, d->sub->targetList);
		bool		grouped_already = false;

		foreach_node(TargetEntry, te, g->targetList)
			grouped_already |= equal(te->expr, e);
		if (!grouped_already)
		{
			TargetEntry *te = makeTargetEntry((Expr *) copyObject(e), list_length(g->targetList) + 1, psprintf("g%d", list_length(g->targetList) + 1), false);
			SortGroupClause *c = copyObject(sgc);

			g->targetList = lappend(g->targetList, te);
			te->ressortgroupref = list_length(g->groupClause) + 1;
			c->tleSortGroupRef = te->ressortgroupref;
			g->groupClause = lappend(g->groupClause, c);
		}
	}
	if (d->exists)
	{
		/* the keys of the rows there are, and a column that is true: NULL where the outer row has none */
		g->targetList = lappend(g->targetList, makeTargetEntry((Expr *) makeBoolConst(true, false), list_length(g->targetList) + 1, "found", false));
		*exprs = list_make1(makeTargetEntry((Expr *) makeVar(grti, list_length(g->targetList), BOOLOID, -1, InvalidOid, 0), 1, "found", false));
		return g;
	}
	foreach_node(TargetEntry, te, d->sub->targetList)
	{
		if (te->resjunk)
			continue;
		if (d->groups == NIL)
			*exprs = lappend(*exprs, makeTargetEntry((Expr *) decor_column((Node *) te->expr, &o), te->resno, te->resname, false));
		else
		{
			/* a missing group is a row of NULLs: its columns are the sub-select's own, not expressions of the aggregates evaluated above the join */
			g->targetList = lappend(g->targetList, makeTargetEntry((Expr *) copyObject(te->expr), list_length(g->targetList) + 1, psprintf("o%d", te->resno), false));
			*exprs = lappend(*exprs, makeTargetEntry((Expr *) makeVar(grti, list_length(g->targetList), exprType((Node *) te->expr), exprTypmod((Node *) te->expr),
																	   exprCollation((Node *) te->expr), 0), te->resno, te->resname, false));
		}
	}
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

/* the outer columns come from the items of the FROM tree and are not another lateral sub-select's, which the rewrite replaces (and the rewrite of this one would read stale) */
static bool
decor_outer_ok(Query *q, Decor *d, Node *from)
{
	int			rti = -1;

	if (!bms_is_subset(d->outer, tree_rtis(from)))
		return false;
	while ((rti = bms_next_member(d->outer, rti)) >= 0)
		if (rt_fetch(rti, q->rtable)->rtekind == RTE_SUBQUERY && rt_fetch(rti, q->rtable)->lateral)
			return false;
	return true;
}

/* `x [LEFT | INNER] JOIN LATERAL (aggregates of a table tied to x) ON true` */
bool
decorrelate_join(Query *q, JoinExpr *j, bool apply, List **decor)
{
	RangeTblEntry *rte;
	DecorJoin  *r = palloc0(sizeof(DecorJoin));

	if ((j->jointype != JOIN_LEFT && j->jointype != JOIN_INNER) || !IsA(j->rarg, RangeTblRef) || !true_or_missing(j->quals))
		return false;
	r->lrti = ((RangeTblRef *) j->rarg)->rtindex;
	rte = rt_fetch(r->lrti, q->rtable);
	if (rte->rtekind != RTE_SUBQUERY || !rte->lateral || !decor_analyze(rte->subquery, &r->d) || !decor_outer_ok(q, &r->d, j->larg))
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

/* a column of the lateral sub-select read where the join nulls it (PostgreSQL 19 hands the callback a const Var) */
static Node *
decor_read(SIFT_CALLBACK_VAR *var, replace_rte_variables_context *context)
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
void
decor_apply(Query *q, DecorJoin *r)
{
	List	   *exprs = NIL,
			   *join = NIL,
			   *names = NIL;
	Query	   *g = decor_group(&r->d, r->lrti, NULL, &exprs, &join);
	RangeTblEntry *rte;
	JoinExpr   *j;
	DecorRead	read;
	bool		grouped = r->d.groups != NIL;

	j = join_of((Node *) q->jointree, r->lrti);
	/* one row for every outer row, so a join that did not null the columns now does; the groups of the sub-select keep an inner join an inner join */
	read.exprs = exprs;
	read.nulled = grouped && j->jointype == JOIN_INNER ? NULL : bms_make_singleton(j->rtindex);
	*q = *(Query *) replace_rte_variables((Node *) q, r->lrti, 0, decor_read, &read, NULL);
	rte = rt_fetch(r->lrti, q->rtable);
	rte->subquery = g;
	rte->lateral = false;
	foreach_node(TargetEntry, te, g->targetList)
		names = lappend(names, makeString(te->resname));
	rte->eref = makeAlias(rte->eref->aliasname, names);
	j = join_of((Node *) q->jointree, r->lrti);
	if (!grouped)
	{
		j->jointype = JOIN_LEFT;
		rt_fetch(j->rtindex, q->rtable)->jointype = JOIN_LEFT;
	}
	j->quals = (Node *) make_ands_explicit(join);
	sift_agg_by_cost = true;
}

/* the columns of a FROM item, as a join's range table entry holds them */
static void
join_columns(Query *q, Node *n, List **names, List **vars)
{
	Index		rti = IsA(n, RangeTblRef) ? ((RangeTblRef *) n)->rtindex : ((JoinExpr *) n)->rtindex;
	List	   *nn,
			   *vv;

	sift_expand_rte(rt_fetch(rti, q->rtable), rti, &nn, &vv);
	*names = list_concat(*names, nn);
	*vars = list_concat(*vars, vv);
}

/* the sub-select as a range table entry of the query (its columns named as its select list does) */
static void
add_subquery(Query *q, Query *sub)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	List	   *names = NIL;

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = sub;
	rte->inFromCl = true;
	foreach_node(TargetEntry, te, sub->targetList)
		names = lappend(names, makeString(te->resname));
	rte->eref = makeAlias("decor", names);
	q->rtable = lappend(q->rtable, rte);
}

/* `left JOIN rti ON quals` with the range table entry of the join that the planner expects */
static JoinExpr *
join_item(Query *q, Node *left, Index rti, JoinType type, Node *quals)
{
	List	   *names = NIL,
			   *vars = NIL;
	RangeTblRef *ref = makeNode(RangeTblRef);
	JoinExpr   *j = makeNode(JoinExpr);
	RangeTblEntry *jr = makeNode(RangeTblEntry);
	int			nleft;

	join_columns(q, left, &names, &vars);
	nleft = list_length(names);
	ref->rtindex = rti;
	j->jointype = type;
	j->larg = left;
	j->rarg = (Node *) ref;
	j->quals = quals;
	join_columns(q, (Node *) ref, &names, &vars);
	jr->rtekind = RTE_JOIN;
	jr->jointype = type;
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
	return j;
}

/* `(SELECT aggregate FROM t WHERE t.k = x.k)` of a select list or WHERE: the item of the FROM list that holds x LEFT JOIN the table grouped */
static Node *
decor_sublink(Node *n, SublinkDecor *c)
{
	SubLink    *s;
	Decor		d = {0};
	Query	   *q = c->q;
	List	   *exprs = NIL,
			   *join = NIL;
	ListCell   *item = NULL;
	Index		grti = list_length(q->rtable) + 1;
	Node	   *e;

	if (n == NULL)
		return NULL;
	SIFT_SKIP_CONST_LIST(n, n);
	if (!IsA(n, SubLink))
		return expression_tree_mutator(n, decor_sublink, c);
	s = (SubLink *) n;
	if ((s->subLinkType != EXPR_SUBLINK && s->subLinkType != EXISTS_SUBLINK) || list_member_ptr(c->semi, s))
		return n;
	d.exists = s->subLinkType == EXISTS_SUBLINK;
	if (!decor_analyze(castNode(Query, s->subselect), &d) || d.groups != NIL)
		return n;
	foreach(item, q->jointree->fromlist)
		if (bms_is_subset(d.outer, tree_rtis(lfirst(item))))
			break;
	if (item == NULL)
		return n;
	c->changed = true;
	if (!c->apply)
		return n;
	add_subquery(q, decor_group(&d, grti, bms_make_singleton(grti + 1), &exprs, &join));
	sift_agg_by_cost = true;
	lfirst(item) = join_item(q, lfirst(item), grti, JOIN_LEFT, (Node *) make_ands_explicit(join));
	e = (Node *) linitial_node(TargetEntry, exprs)->expr;
	if (d.exists)
	{
		/* a group is there, or the left join made up a row of NULLs: never NULL, as EXISTS is not */
		NullTest   *t = makeNode(NullTest);

		t->arg = (Expr *) add_nulling_relids(e, NULL, bms_make_singleton(grti + 1));
		t->nulltesttype = IS_NOT_NULL;
		t->location = -1;
		return (Node *) t;
	}
	return e;
}

/* the sub-selects the WHERE ANDs together, EXISTS and NOT EXISTS among them */
static void
semi_sublinks(Node *quals, List **semi)
{
	if (quals == NULL)
		return;
	if (IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR)
		foreach_ptr(Node, a, ((BoolExpr *) quals)->args)
			semi_sublinks(a, semi);
	else if (IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == NOT_EXPR && IsA(linitial(((BoolExpr *) quals)->args), SubLink))
		semi_sublinks(linitial(((BoolExpr *) quals)->args), semi);
	else if (IsA(quals, SubLink) && ((SubLink *) quals)->subLinkType == EXISTS_SUBLINK)
		*semi = lappend(*semi, quals);
}

bool
decorrelate_sublinks(Query *q, bool apply)
{
	SublinkDecor c = {q, apply, false, NIL};

	if (!q->hasSubLinks || q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->groupClause != NIL || q->groupingSets != NIL ||
		q->havingQual != NULL || q->jointree == NULL || q->setOperations != NULL)
		return false;
	semi_sublinks(q->jointree->quals, &c.semi);
	foreach_node(TargetEntry, te, q->targetList)
		te->expr = (Expr *) decor_sublink((Node *) te->expr, &c);
	q->jointree->quals = decor_sublink(q->jointree->quals, &c);
	return c.changed;
}

/* `FROM x, LATERAL (aggregates of a table tied to x)` is that sub-select inner joined to the item of the list that holds the columns it reads */
bool
decorrelate_comma(Query *q, FromExpr *f, bool apply, List **decor)
{
	bool		changed = false;
	int			i = 1;

	while (i < list_length(f->fromlist))
	{
		Node	   *n = list_nth(f->fromlist, i);
		RangeTblEntry *rte;
		Decor		d = {0};
		int			k = 0;

		if (IsA(n, RangeTblRef) && (rte = rt_fetch(((RangeTblRef *) n)->rtindex, q->rtable))->rtekind == RTE_SUBQUERY && rte->lateral &&
			decor_analyze(rte->subquery, &d))
		{
			while (k < i && !decor_outer_ok(q, &d, list_nth(f->fromlist, k)))
				k++;
			if (k < i)
			{
				changed = true;
				if (apply)
				{
					JoinExpr   *j = join_item(q, list_nth(f->fromlist, k), ((RangeTblRef *) n)->rtindex, JOIN_INNER, NULL);

					lfirst(list_nth_cell(f->fromlist, k)) = j;
					f->fromlist = list_delete_nth_cell(f->fromlist, i);
					decorrelate_join(q, j, true, decor);
					continue;
				}
			}
		}
		i++;
	}
	return changed;
}
