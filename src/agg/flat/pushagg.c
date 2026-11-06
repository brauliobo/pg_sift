/*
 * agg/flat/pushagg.c - an aggregate over a UNION ALL is added up from the groups of each arm
 * `SELECT bucket(ev.ts), ev.kind, sum(ev.v) FROM (SELECT t0 AS ts, kind, 1 AS v FROM events WHERE .. UNION ALL SELECT t1, kind, -1 FROM events WHERE ..) ev GROUP BY 1, 2`
 * hands every row of every arm to one grouping above the union. Each arm groups its own rows by the keys and returns the partial aggregates of its groups; the grouping
 * above adds them up: a sum of sums, a sum of counts, a min of mins, a max of maxes (the aggregates that are the same whatever the groups they are made from, so not
 * avg, DISTINCT, FILTER, an ordered aggregate, nor a float sum, whose answer depends on the order of the additions). An arm that reads a table with a column index
 * then runs in the scan, grouped by the keys it computes there. A count or a sum of ints comes back as the numeric that adding up int8 gives, cast to the bigint it was.
 * The rewrite is chosen by cost like the other flat ones: where the arms return a row for each group already, the grouping of each arm only repeats the one above. An
 * arm that the plan does not run in the scan (the normal plan is cheaper for it) is not worth the grouping either: the hash aggregate above the union, which PostgreSQL
 * runs in its parallel workers, groups its rows once. The planner hook plans the rewrite with every arm grouped, reads from the plan which arms run in the scan, and
 * plans again with the others as they were, each returning the partial aggregate of its one row (the value, 1 for a count).
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

int			sift_agg_push_arms;	/* the arms of the last rewrite that grouped them */
Bitmapset  *sift_agg_push_raw;	/* the arms (by position) the next rewrite leaves ungrouped */

typedef struct Push
{
	Index		rti;			/* the entry of the union */
	RangeTblEntry *ev;
	Index		grti;			/* the entry of the grouping step (PostgreSQL 18; 0 before) */
	List	   *keys;			/* the group expressions, over the union's columns */
	List	   *aggs;			/* the aggregates of the query, each once */
	List	   *arms;			/* the entries of the selects the union is made of */
} Push;

/* a node of an expression over the union's columns only: the union's Vars and nothing that reads another level or runs a query */
static bool
over_union_walker(Node *n, Push *p)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
		return ((Var *) n)->varno != p->rti || ((Var *) n)->varlevelsup != 0;
	if (IsA(n, Aggref) || IsA(n, GroupingFunc) || IsA(n, WindowFunc) || IsA(n, SubLink) || IsA(n, Query) || IsA(n, PlaceHolderVar))
		return true;
	return expression_tree_walker(n, over_union_walker, p);
}

static bool
over_union(Node *n, Push *p)
{
	return !over_union_walker(n, p) && !contain_volatile_functions(n);
}

/* the aggregates that add up as sums of bigint or numeric: counts and sums of ints and numerics */
static bool
is_total(Oid f)
{
	return f == F_COUNT_ANY || f == F_COUNT_ || f == F_SUM_INT2 || f == F_SUM_INT4 || f == F_SUM_INT8 || f == F_SUM_NUMERIC;
}

/* the aggregates whose answer is the same added up from the groups of any partition of the rows */
bool
flat_addable(Aggref *a)
{
	char	   *name;

	if (a->agglevelsup != 0 || a->aggkind != AGGKIND_NORMAL || a->aggdistinct != NIL || a->aggorder != NIL || a->aggfilter != NULL || a->aggdirectargs != NIL || a->aggvariadic)
		return false;
	if (is_total(a->aggfnoid))
		return true;
	if (get_func_namespace(a->aggfnoid) != PG_CATALOG_NAMESPACE || list_length(a->aggargtypes) != 1)
		return false;
	name = get_func_name(a->aggfnoid);
	return name != NULL && (strcmp(name, "min") == 0 || strcmp(name, "max") == 0);
}

typedef struct Collect
{
	Push	   *p;
	bool		ok;
} Collect;

/* the aggregates of the select list and the HAVING, each once (the same aggregate written twice is added up once) */
static bool
collect_walker(Node *n, Collect *c)
{
	if (n == NULL || !c->ok)
		return false;
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;

		if (!flat_addable(a) || !over_union((Node *) a->args, c->p))
			c->ok = false;
		else if (!list_member(c->p->aggs, a))
			c->p->aggs = lappend(c->p->aggs, a);
		return false;
	}
	if (IsA(n, SubLink) || IsA(n, Query))
		c->ok = false;
	return expression_tree_walker(n, collect_walker, c);
}

/* the select the arm is: rows of its tables and expressions of them, with nothing that groups, orders, limits or repeats rows */
static bool
plain_arm(Query *a)
{
	return a->commandType == CMD_SELECT && a->setOperations == NULL && !a->hasAggs && !a->hasWindowFuncs && !a->hasTargetSRFs && !a->hasDistinctOn && !a->hasModifyingCTE &&
		a->groupClause == NIL && a->groupingSets == NIL && a->havingQual == NULL && a->distinctClause == NIL && a->windowClause == NIL && a->sortClause == NIL &&
		a->limitCount == NULL && a->limitOffset == NULL && a->rowMarks == NIL;
}

/* the selects of a tree of UNION ALL (nothing else removes or repeats rows) */
static bool
union_arms(Query *u, Node *n, List **arms)
{
	if (IsA(n, RangeTblRef))
	{
		RangeTblEntry *r = rt_fetch(((RangeTblRef *) n)->rtindex, u->rtable);

		if (r->rtekind != RTE_SUBQUERY || r->lateral || r->security_barrier || !plain_arm(r->subquery))
			return false;
		*arms = lappend(*arms, r);
		return true;
	}
	if (IsA(n, SetOperationStmt))
	{
		SetOperationStmt *s = (SetOperationStmt *) n;

		return s->op == SETOP_UNION && s->all && union_arms(u, s->larg, arms) && union_arms(u, s->rarg, arms);
	}
	return false;
}

/* is a Var of the union, or of the grouping step, left in the query outside the aggregates and the group expressions? (the rewrite would leave it reading columns that are gone) */
static bool
left_walker(Node *n, Push *p)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Aggref))
		return false;
#if PG_VERSION_NUM < 180000
	if (!IsA(n, TargetEntry) && !IsA(n, List) && list_member(p->keys, n))
		return false;
#endif
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varno == p->rti;
	return expression_tree_walker(n, left_walker, p);
}

/* what the query has to be for the rewrite, or NULL */
static Push *
analyze(Query *q)
{
	Push	   *p;
	Collect		c;
	Query	   *u;
	RangeTblRef *ref;
	bool		indexed = false;
	int			nrte = 0;

	if (q->commandType != CMD_SELECT || !q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasDistinctOn || q->hasSubLinks || q->groupClause == NIL || q->groupingSets != NIL ||
		q->groupDistinct || q->distinctClause != NIL || q->windowClause != NIL || q->rowMarks != NIL || q->jointree == NULL || q->jointree->quals != NULL ||
		list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return NULL;
	p = palloc0_object(Push);
	ref = linitial(q->jointree->fromlist);
	p->rti = ref->rtindex;
	p->ev = rt_fetch(p->rti, q->rtable);
	foreach_node(RangeTblEntry, r, q->rtable)
	{
		nrte++;
#if PG_VERSION_NUM >= 180000
		if (r->rtekind == RTE_GROUP)
		{
			p->grti = nrte;
			continue;
		}
#endif
		if (r != p->ev)
			return NULL;
	}
	if (p->ev->rtekind != RTE_SUBQUERY || p->ev->lateral || p->ev->security_barrier)
		return NULL;
	u = p->ev->subquery;
	if (u->setOperations == NULL || u->sortClause != NIL || u->limitCount != NULL || u->limitOffset != NULL || u->rowMarks != NIL || u->hasAggs || u->groupClause != NIL ||
		!union_arms(u, u->setOperations, &p->arms))
		return NULL;
	foreach_node(RangeTblEntry, r, p->arms)
		foreach_node(RangeTblEntry, t, r->subquery->rtable)
			indexed |= t->rtekind == RTE_RELATION && t->relkind == RELKIND_RELATION && has_column_source(t->relid);
	if (!indexed)
		return NULL;

	foreach_node(SortGroupClause, g, q->groupClause)
	{
		Node	   *key = (Node *) get_sortgroupclause_tle(g, q->targetList)->expr;

#if PG_VERSION_NUM >= 180000
		if (!IsA(key, Var) || ((Var *) key)->varno != p->grti)
			return NULL;
		key = list_nth(rt_fetch(p->grti, q->rtable)->groupexprs, ((Var *) key)->varattno - 1);
#endif
		if (!over_union(key, p))
			return NULL;
		p->keys = lappend(p->keys, key);
	}
	c.p = p;
	c.ok = true;
	collect_walker((Node *) q->targetList, &c);
	collect_walker(q->havingQual, &c);
	if (!c.ok || p->aggs == NIL || left_walker((Node *) q->targetList, p) || left_walker(q->havingQual, p))
		return NULL;
	return p;
}

/* the union's columns: the group expressions, then the partial aggregates */
static Var *
union_column(Push *p, int i)
{
	Node	   *e = i < list_length(p->keys) ? list_nth(p->keys, i) : (Node *) list_nth(p->aggs, i - list_length(p->keys));

	return makeVar(p->rti, i + 1, exprType(e), exprTypmod(e), exprCollation(e), 0);
}

/* the position of the node in the list (equal nodes are one) */
static int
index_of(List *l, Node *n)
{
	int			i = 0;

	foreach_ptr(Node, x, l)
	{
		if (equal(x, n))
			return i;
		i++;
	}
	return -1;
}

/* the aggregate that adds up the partial aggregates of the aggregate a, which the expression part holds for each group */
Node *
flat_add_up(Aggref *a, Node *part)
{
	Aggref	   *o = makeNode(Aggref);

	o->aggfnoid = a->aggfnoid;
	o->aggtype = a->aggtype;
	o->aggcollid = a->aggcollid;
	o->inputcollid = a->inputcollid;
	if (is_total(a->aggfnoid))
	{
		/* adding up bigints (the sums and counts of ints) or numerics: PostgreSQL's sums of int8 and of numeric give a numeric */
		o->aggfnoid = a->aggtype == INT8OID ? F_SUM_INT8 : F_SUM_NUMERIC;
		o->aggtype = NUMERICOID;
		o->aggcollid = o->inputcollid = InvalidOid;
	}
	o->aggargtypes = list_make1_oid(exprType(part));
	o->args = list_make1(makeTargetEntry((Expr *) part, 1, NULL, false));
	o->aggkind = AGGKIND_NORMAL;
	o->aggsplit = AGGSPLIT_SIMPLE;
	o->aggno = o->aggtransno = -1;
	o->location = a->location;
	if (o->aggtype == a->aggtype)
		return (Node *) o;
	return coerce_to_target_type(NULL, (Node *) o, o->aggtype, a->aggtype, -1, COERCION_EXPLICIT, COERCE_IMPLICIT_CAST, -1);
}

static Node *
outer_aggregate(Push *p, Aggref *a)
{
	return flat_add_up(a, (Node *) union_column(p, list_length(p->keys) + index_of(p->aggs, (Node *) a)));
}

static Node *
outer_mutator(Node *n, Push *p)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
		return outer_aggregate(p, (Aggref *) n);
#if PG_VERSION_NUM < 180000
	if (!IsA(n, TargetEntry) && !IsA(n, List))
	{
		int			i = index_of(p->keys, n);

		if (i >= 0)
			return (Node *) union_column(p, i);
	}
#endif
	return expression_tree_mutator(n, outer_mutator, p);
}

/* the types of the union's columns, for the tree of set operations over its arms */
static void
set_column_types(Node *n, List *types, List *typmods, List *collations)
{
	SetOperationStmt *s;

	if (!IsA(n, SetOperationStmt))
		return;
	s = (SetOperationStmt *) n;
	s->colTypes = types;
	s->colTypmods = typmods;
	s->colCollations = collations;
	set_column_types(s->larg, types, typmods, collations);
	set_column_types(s->rarg, types, typmods, collations);
}

/* the partial aggregate of one row, for an arm that does not group: its value (cast to the aggregate's type), a count of 1 or of whether the value is there */
static Expr *
row_partial(Aggref *a)
{
	Expr	   *arg = a->args == NIL ? NULL : linitial_node(TargetEntry, a->args)->expr;
	CaseExpr   *c;
	CaseWhen   *w;
	NullTest   *t;

	if (a->aggfnoid == F_COUNT_)
		return (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(1), false, FLOAT8PASSBYVAL);
	if (a->aggfnoid != F_COUNT_ANY)
		return (Expr *) coerce_to_target_type(NULL, (Node *) arg, exprType((Node *) arg), a->aggtype, -1, COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	t = makeNode(NullTest);
	t->arg = arg;
	t->nulltesttype = IS_NOT_NULL;
	t->location = -1;
	w = makeNode(CaseWhen);
	w->expr = (Expr *) t;
	w->result = (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(1), false, FLOAT8PASSBYVAL);
	w->location = -1;
	c = makeNode(CaseExpr);
	c->casetype = INT8OID;
	c->args = list_make1(w);
	c->defresult = (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, FLOAT8PASSBYVAL);
	c->location = -1;
	return (Expr *) c;
}

/* each arm returns the group expressions and the partial aggregates, grouped by the former; the union and the entries of the arms are renamed to match */
static void
rewrite_arms(Push *p)
{
	Query	   *u = p->ev->subquery;
	int			nkeys = list_length(p->keys),
				ncols = nkeys + list_length(p->aggs);
	List	   *names = NIL,
			   *top = NIL,
			   *types = NIL,
			   *typmods = NIL,
			   *collations = NIL;

	for (int i = 0; i < ncols; i++)
	{
		Var		   *v = union_column(p, i);
		char	   *name = psprintf(i < nkeys ? "k%d" : "a%d", i < nkeys ? i + 1 : i - nkeys + 1);

		names = lappend(names, makeString(name));
		top = lappend(top, makeTargetEntry((Expr *) makeVar(1, i + 1, v->vartype, v->vartypmod, v->varcollid, 0), i + 1, name, false));
		types = lappend_oid(types, v->vartype);
		typmods = lappend_int(typmods, v->vartypmod);
		collations = lappend_oid(collations, v->varcollid);
	}
	foreach_node(RangeTblEntry, r, p->arms)
	{
		Query	   *a = r->subquery;
		List	   *tlist = NIL;
		int			resno = 1;
		bool		raw = bms_is_member(foreach_current_index(r), sift_agg_push_raw);

		for (int i = 0; i < ncols; i++)
		{
			Node	   *e = i < nkeys ? list_nth(p->keys, i) : (Node *) list_nth(p->aggs, i - nkeys);

			e = sift_replace_vars_from_targetlist(e, p->rti, p->ev, a->targetList);
			if (raw && i >= nkeys)
				e = (Node *) row_partial((Aggref *) e);
			tlist = lappend(tlist, makeTargetEntry((Expr *) e, resno++, strVal(list_nth(names, i)), false));
		}
		a->targetList = tlist;
		if (!raw)
		{
			a->hasAggs = true;
			for (int i = 0; i < nkeys; i++)
				group_by(a, list_nth(tlist, i));
		}
		r->eref = makeAlias(r->eref->aliasname, names);
	}
	u->targetList = top;
	set_column_types(u->setOperations, types, typmods, collations);
	p->ev->eref = makeAlias(p->ev->eref->aliasname, names);
	if (p->ev->alias != NULL)
		p->ev->alias = makeAlias(p->ev->alias->aliasname, NIL);
}

bool
sift_agg_push_partial_aggs(Query *q, bool apply)
{
	Push	   *p = analyze(q);

	if (p == NULL)
		return false;
	if (!apply)
		return true;
	/* the new columns are made from the old ones, so the outer query is rewritten before the arms are */
	q->targetList = (List *) outer_mutator((Node *) q->targetList, p);
	q->havingQual = outer_mutator(q->havingQual, p);
#if PG_VERSION_NUM >= 180000
	{
		RangeTblEntry *g = rt_fetch(p->grti, q->rtable);
		List	   *exprs = NIL;

		for (int i = 0; i < list_length(p->keys); i++)
			exprs = lappend(exprs, union_column(p, i));
		g->groupexprs = exprs;
	}
#endif
	rewrite_arms(p);
	sift_agg_push_arms = list_length(p->arms);
	sift_agg_by_cost = true;
	return true;
}
