/*
 * agg/flat/dsplit.c - distinct counts of one key under filters of its own columns share one set
 * `SELECT g, count(DISTINCT p.id), count(DISTINCT p.id) FILTER (WHERE f1(p)), count(DISTINCT p.id) FILTER (WHERE f2(p)) FROM c JOIN p ON p.id = c.k GROUP BY g` keeps
 * a set of ids for each of the three. The id is a unique key, so it determines the row of p and with it the value of f1 and f2: the sub-select groups by g and by f1 and
 * f2 and counts the ids once, and the query above adds up the counts of the groups a filter keeps. An id is in one group of a g, so the sums are the distinct counts (a
 * group the filter keeps nothing of adds 0, not NULL). The other aggregates are counts of rows, or distinct counts of the unique key of a table that joins p by its key,
 * which determines p too: the sub-select computes them as they are for each group and the query above adds them up. The planner hook plans the query both ways and keeps
 * this one if it runs in the scan and costs less.
 */
#include "postgres.h"

#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

#define MAX_FILTERS 16
#define FLAT_SPLIT_MARGIN 1.0

typedef struct Split
{
	Query	   *q;				/* the query (a copy: the rewrite changes it) */
	Index		p;				/* the entry whose unique key the filtered distinct counts count */
	AttrNumber	pkatt;
	List	   *rtis;
	List	   *quals;
	List	   *gexprs;
	List	   *keys;			/* the unique keys the distinct counts count (Var) */
	List	   *filters;		/* the filters of the counts of p's key, each once: the sub-select's boolean columns */
	List	   *parts;			/* the aggregates the sub-select computes, each once: the count of p's key, and the others as they are */
	Aggref	   *keycount;		/* count(DISTINCT p.key) */
	int			ndistinct;		/* the counts of p's key */
} Split;

/* 1: the expression reads columns of p only, 3: and of other tables, 0: of none; -1: it cannot be moved (a sub-select, a volatile function, another level) */
static int
reads_of(Split *s, Node *n)
{
	int			r = 0;

	if (contain_volatile_functions(n) || contain_subplans(n) || checkExprHasSubLink(n))
		return -1;
	foreach_ptr(Var, v, pull_var_clause(n, PVC_RECURSE_PLACEHOLDERS))
	{
		if (v->varlevelsup != 0 || rt_fetch(v->varno, s->q->rtable)->rtekind != RTE_RELATION)
			return -1;
		r |= v->varno == s->p ? 1 : 2;
	}
	return r;
}

static Var *
plain_var(Node *n)
{
	n = strip_relabel(n);
	return IsA(n, Var) && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varattno > 0 ? (Var *) n : NULL;
}

static bool
unique_var(Split *s, Var *v)
{
	RangeTblEntry *rte = rt_fetch(v->varno, s->q->rtable);

	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && sift_agg_unique_key(rte->relid, 1, 0, &(int){v->varattno}, NULL);
}

/* the unique key x of p, or of a table that an equality of the statement joins to p's key */
static bool
determines_p(Split *s, Var *x)
{
	if (x->varno == s->p)
		return x->varattno == s->pkatt;
	foreach_ptr(Node, c, s->quals)
	{
		OpExpr	   *op;
		Var		   *l,
				   *r;

		if (!IsA(c, OpExpr) || list_length((op = (OpExpr *) c)->args) != 2)
			continue;
		l = plain_var(linitial(op->args));
		r = plain_var(lsecond(op->args));
		if (l == NULL || r == NULL || l->vartype != r->vartype || lookup_type_cache(l->vartype, TYPECACHE_EQ_OPR)->eq_opr != op->opno)
			continue;
		if ((l->varno == s->p && l->varattno == s->pkatt && r->varno == x->varno) || (r->varno == s->p && r->varattno == s->pkatt && l->varno == x->varno))
			return true;
	}
	return false;
}

static int
list_index_equal(List **l, Node *x)
{
	int			i = 0;

	foreach_ptr(Node, y, *l)
	{
		if (equal(x, y))
			return i;
		i++;
	}
	*l = lappend(*l, x);
	return i;
}

/* the column of a count(DISTINCT x) of a unique key x: x */
static Var *
distinct_key(Aggref *a)
{
	if (a->aggfnoid != F_COUNT_ANY || list_length(a->args) != 1 || a->aggdistinct == NIL)
		return NULL;
	return plain_var((Node *) linitial_node(TargetEntry, a->args)->expr);
}

static bool
counts_p_key(Split *s, Aggref *a)
{
	Var		   *x = distinct_key(a);

	return x != NULL && x->varno == s->p && x->varattno == s->pkatt;
}

/* the first pass: every aggregate is a count, the distinct ones of a unique key; the keys are noted */
static bool
kinds_walker(Node *n, Split *s)
{
	Aggref	   *a;
	Var		   *x;

	if (n == NULL)
		return false;
	if (!IsA(n, Aggref))
		return expression_tree_walker(n, kinds_walker, s);
	a = (Aggref *) n;
	if (a->agglevelsup != 0 || a->aggkind != AGGKIND_NORMAL || a->aggorder != NIL || (a->aggfnoid != F_COUNT_ && a->aggfnoid != F_COUNT_ANY))
		return true;
	if (a->aggfilter != NULL && reads_of(s, (Node *) a->aggfilter) < 0)
		return true;
	if (a->aggdistinct == NIL)
		return !a->aggstar && reads_of(s, (Node *) linitial_node(TargetEntry, a->args)->expr) < 0;
	if ((x = distinct_key(a)) == NULL || !unique_var(s, x))
		return true;
	foreach_ptr(Var, k, s->keys)
		if (k->varno == x->varno && k->varattno == x->varattno)
			return false;
	s->keys = lappend(s->keys, x);
	return false;
}

/* p: the table whose key the other counted keys' tables determine; the filtered counts of it read it alone, and there are two or more */
static bool
count_walker(Node *n, Split *s)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;

		if (counts_p_key(s, a))
		{
			s->ndistinct++;
			return a->aggfilter != NULL && reads_of(s, (Node *) a->aggfilter) != 1;
		}
		return false;
	}
	return expression_tree_walker(n, count_walker, s);
}

/* the sub-select's filters and parts, in the order the query reads them */
static bool
fill_walker(Node *n, Split *s)
{
	Aggref	   *a;

	if (n == NULL)
		return false;
	if (!IsA(n, Aggref))
		return expression_tree_walker(n, fill_walker, s);
	a = (Aggref *) n;
	if (counts_p_key(s, a))
	{
		if (a->aggfilter != NULL)
			list_index_equal(&s->filters, (Node *) a->aggfilter);
		if (s->keycount == NULL)
		{
			s->keycount = copyObject(a);
			s->keycount->aggfilter = NULL;
		}
		list_index_equal(&s->parts, (Node *) s->keycount);
	}
	else
		list_index_equal(&s->parts, (Node *) a);
	return false;
}

/* does the expression read a column outside the aggregates and the expressions the query groups by? */
static bool
outside_walker(Node *n, Split *s)
{
	if (n == NULL || IsA(n, Aggref))
		return false;
	foreach_ptr(Node, g, s->gexprs)
		if (equal(n, g))
			return false;
	if (IsA(n, Var) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar))
		return true;
	return expression_tree_walker(n, outside_walker, s);
}

static bool
has_filtered_distinct_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		return ((Aggref *) n)->aggdistinct != NIL && ((Aggref *) n)->aggfilter != NULL;
	return expression_tree_walker(n, has_filtered_distinct_walker, context);
}

static bool
analyze(Split *s)
{
	Query	   *q = s->q;
	int			nrels = 0;

	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind != RTE_JOIN)
			nrels += rte->rtekind == RTE_RELATION ? 1 : 1000;
	flat_inner_items((Node *) q->jointree, &s->rtis, &s->quals);
	if (nrels != list_length(s->rtis) || nrels < 2 || q->groupClause == NIL)
		return false;
	foreach_node(SortGroupClause, sgc, q->groupClause)
	{
		TargetEntry *te = get_sortgroupclause_tle(sgc, q->targetList);

		if (contain_volatile_functions((Node *) te->expr) || contain_subplans((Node *) te->expr) || checkExprHasSubLink((Node *) te->expr))
			return false;
		s->gexprs = lappend(s->gexprs, te->expr);
	}
	foreach_node(TargetEntry, te, q->targetList)
		if (kinds_walker((Node *) te->expr, s) || outside_walker((Node *) te->expr, s))
			return false;
	if (kinds_walker(q->havingQual, s) || outside_walker(q->havingQual, s))
		return false;
	foreach_ptr(Var, k, s->keys)
	{
		bool		all = true;

		s->p = k->varno;
		s->pkatt = k->varattno;
		foreach_ptr(Var, other, s->keys)
			all &= determines_p(s, other);
		if (all)
			break;
		s->p = 0;
	}
	if (s->p == 0)
		return false;
	foreach_node(TargetEntry, te, q->targetList)
		if (count_walker((Node *) te->expr, s))
			return false;
	if (count_walker(q->havingQual, s) || s->ndistinct < 2)
		return false;
	foreach_node(TargetEntry, te, q->targetList)
		fill_walker((Node *) te->expr, s);
	fill_walker(q->havingQual, s);
	return s->filters != NIL && list_length(s->filters) <= MAX_FILTERS;
}

static RangeTblRef *
rtref(int rti)
{
	RangeTblRef *r = makeNode(RangeTblRef);

	r->rtindex = rti;
	return r;
}

/* the sub-select's columns: the groups, then the filters, then the parts */
static Var *
filter_var(Split *s, Node *f)
{
	return makeVar(1, list_length(s->gexprs) + 1 + list_index_equal(&s->filters, f), BOOLOID, -1, InvalidOid, 0);
}

static Var *
part_var(Split *s, Node *a)
{
	return makeVar(1, list_length(s->gexprs) + list_length(s->filters) + 1 + list_index_equal(&s->parts, a), INT8OID, -1, InvalidOid, 0);
}

/* the bigint sum of a bigint column of the sub-select (0 for a group the filter keeps no row of): what the count was */
static Node *
sum_of(Var *col, Var *filter)
{
	Aggref	   *a = makeNode(Aggref);
	CoalesceExpr *c = makeNode(CoalesceExpr);

	a->aggfnoid = F_SUM_INT8;
	a->aggtype = NUMERICOID;
	a->aggargtypes = list_make1_oid(INT8OID);
	a->args = list_make1(makeTargetEntry((Expr *) col, 1, NULL, false));
	a->aggfilter = (Expr *) filter;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	c->coalescetype = INT8OID;
	c->args = list_make2(makeFuncExpr(F_INT8_NUMERIC, INT8OID, list_make1(a), InvalidOid, InvalidOid, COERCE_EXPLICIT_CAST),
						 makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, FLOAT8PASSBYVAL));
	c->location = -1;
	return (Node *) c;
}

static Node *
outer_mutator(Node *n, Split *s)
{
	int			i = 0;

	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;

		if (counts_p_key(s, a))
			return sum_of(part_var(s, (Node *) s->keycount), a->aggfilter == NULL ? NULL : filter_var(s, (Node *) a->aggfilter));
		return sum_of(part_var(s, n), NULL);
	}
	foreach_ptr(Node, g, s->gexprs)
	{
		i++;
		if (equal(n, g))
			return (Node *) makeVar(1, i, exprType(g), exprTypmod(g), exprCollation(g), 0);
	}
	return expression_tree_mutator(n, outer_mutator, s);
}

/* the tables as written, grouped by the keys of the query and the filters, with the count of p's key and the other aggregates of each group */
static Query *
inner_query(Split *s)
{
	Query	   *q = s->q,
			   *r = makeNode(Query);
	List	   *froms = NIL;
	int			resno = 1;

	r->commandType = CMD_SELECT;
	r->querySource = QSRC_ORIGINAL;
	r->canSetTag = true;
	r->rtable = copyObject(q->rtable);
	r->rteperminfos = copyObject(q->rteperminfos);
	foreach_int(rti, s->rtis)
		froms = lappend(froms, rtref(rti));
	r->jointree = makeFromExpr(froms, s->quals == NIL ? NULL : list_length(s->quals) == 1 ? copyObject(linitial(s->quals)) :
							   (Node *) makeBoolExpr(AND_EXPR, copyObject(s->quals), -1));
	foreach_node(SortGroupClause, sgc, q->groupClause)
	{
		TargetEntry *te = get_sortgroupclause_tle(sgc, q->targetList),
				   *n = makeTargetEntry((Expr *) copyObject(te->expr), resno, psprintf("g%d", resno), false);

		resno++;
		n->ressortgroupref = te->ressortgroupref;
		r->targetList = lappend(r->targetList, n);
		r->groupClause = lappend(r->groupClause, copyObject(sgc));
	}
	foreach_ptr(Node, f, s->filters)
	{
		TargetEntry *te = makeTargetEntry((Expr *) copyObject(f), resno, psprintf("m%d", resno), false);

		resno++;
		r->targetList = lappend(r->targetList, te);
		group_by(r, te);
	}
	foreach_ptr(Aggref, a, s->parts)
	{
		r->targetList = lappend(r->targetList, makeTargetEntry((Expr *) copyObject(a), resno, psprintf("n%d", resno), false));
		resno++;
	}
	r->hasAggs = true;
	r->hasSubLinks = q->hasSubLinks;
	return r;
}

bool
sift_agg_distinct_split_candidate(Query *q)
{
	return q->commandType == CMD_SELECT && q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs && q->groupClause != NIL && q->groupingSets == NIL &&
		q->distinctClause == NIL && q->setOperations == NULL && q->cteList == NIL && q->rowMarks == NIL && q->jointree != NULL && list_length(q->rtable) >= 2 &&
		has_filtered_distinct_walker((Node *) q->targetList, NULL);
}

bool
sift_agg_distinct_split(Query *q, bool apply)
{
	Split		s = {0};
	Query	   *sub;
	RangeTblEntry *arte;
	List	   *names = NIL;

	if (!sift_agg_distinct_split_candidate(q))
		return false;
	s.q = copyObject(q);
	flat_ungroup(s.q);
	if (!analyze(&s))
		return false;
	if (!apply)
		return true;
	sub = inner_query(&s);
	s.q->targetList = (List *) outer_mutator((Node *) s.q->targetList, &s);
	s.q->havingQual = outer_mutator(s.q->havingQual, &s);
	foreach_node(TargetEntry, te, sub->targetList)
		names = lappend(names, makeString(te->resname));
	arte = makeNode(RangeTblEntry);
	arte->rtekind = RTE_SUBQUERY;
	arte->subquery = sub;
	arte->inFromCl = true;
	arte->eref = makeAlias("split", names);
	s.q->rteperminfos = NIL;
	s.q->hasSubLinks = checkExprHasSubLink((Node *) s.q->targetList) || checkExprHasSubLink(s.q->havingQual);
	s.q->jointree = makeFromExpr(list_make1(rtref(1)), NULL);
	s.q->rtable = list_make1(arte);
	*q = *s.q;
	/* the estimate of the groups of the sub-select is the classes of the table's tests, an upper bound: the plan of one set is kept where it costs less, with no margin */
	if (!sift_agg_flat_any)
		sift_agg_flat_margin = FLAT_SPLIT_MARGIN;
	sift_agg_by_cost = sift_agg_flat_any = true;
	return true;
}
