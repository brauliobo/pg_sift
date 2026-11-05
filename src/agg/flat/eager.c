/*
 * agg/flat/eager.c - a sum of a column times a value looked up by a unique key is added up for each key first
 * `SELECT t.region_id, sum(e.amount * k.rate) FROM events e JOIN regions t ON .. JOIN kinds k ON k.id = e.kind_id GROUP BY 1` multiplies every row of e by the rate of its
 * kind. The kind of a row has one rate (k.id is unique), so the rows of a kind and a group add up to `sum(e.amount)` times that rate: the sub-select groups the rows by the
 * keys of the query and the kind, adds up the amounts, and the query above joins it to kinds and adds up the products of the groups (a few hundred, where the rows are
 * hundreds of thousands). Numeric multiplication is exact, so the sum of the products is the sum of the amounts times the rate: the display scale (the largest of the
 * products', which is that of the largest amount of a kind plus the rate's), NULL (a kind whose amounts or rate are all NULL gives no product) and NaN come out the same. A float
 * sum is not exact and its answer depends on the order of the additions, so it is left as it is. Only inner joins: the rows of a kind that kinds has no row for (or a NULL kind)
 * fall out of the join above as they do of the join below. The planner hook plans the query both ways and keeps this one if it runs in the scan and costs much less.
 */
#include "postgres.h"

#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

/* what an expression reads: columns of the lookup's entry, columns of another table, or something the rewrite does not move (another level, a sub-select, an entry that is not a table) */
typedef struct Reads
{
	List	   *rtable;
	Index		d;
	bool		mine;
	bool		other;
	bool		bad;
} Reads;

static bool
reads_walker(Node *n, Reads *r)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varlevelsup != 0 || rt_fetch(v->varno, r->rtable)->rtekind != RTE_RELATION)
			r->bad = true;
		else if (v->varno == r->d)
			r->mine = true;
		else
			r->other = true;
		return false;
	}
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar) || IsA(n, Query))
	{
		r->bad = true;
		return false;
	}
	return expression_tree_walker(n, reads_walker, r);
}

static Reads
reads(List *rtable, Index d, Node *n)
{
	Reads		r = {rtable, d, false, false, false};

	reads_walker(n, &r);
	return r;
}

typedef struct Eager
{
	Query	   *q;				/* the query as PostgreSQL 17 parses it (a copy: the rewrite changes it) */
	Index		d;				/* the entry of the table looked up */
	Var		   *fv;				/* the column of the other table that equals its key */
	Node	   *join;			/* that equality */
	List	   *rest;			/* the other conditions: none reads the lookup */
	List	   *rtis;			/* the entries of the FROM list */
	List	   *gexprs;			/* the expressions the query groups by */
	List	   *parts;			/* the aggregates the sub-select computes, each once (Part) */
	int			nproducts;		/* the sums of products */
	bool		counts;			/* a count among them */
} Eager;

enum
{
	AGG_NONE,
	AGG_PRODUCT,				/* sum(F * D) */
	AGG_SUM,					/* sum(F) */
	AGG_COUNT					/* count(*), count(F) */
};

/* an aggregate of the sub-select: the sum of F (a sum of F or of a product of F and D) or the count of F (NULL: of the rows), taken from the aggregate it replaces */
typedef struct Part
{
	Aggref	   *proto;
	bool		count;
	Node	   *expr;
} Part;

/*
 * The aggregates this rewrite moves: sum(F * D) or sum(D * F) of numerics, D reading the lookup only and F the other tables (f and dx are the two sides, dright says which is
 * which), and, beside them, sum(F) of a numeric and count(*) or count(F), which the sub-select adds up as they are (a count comes back as the bigint the sum of counts is cast to)
 */
static int
classify(Eager *e, Aggref *a, Node **f, Node **dx, bool *dright)
{
	Node	   *arg;
	Reads		r;

	if (a->agglevelsup != 0 || a->aggdistinct != NIL || a->aggorder != NIL || a->aggfilter != NULL || a->aggkind != AGGKIND_NORMAL)
		return AGG_NONE;
	if (a->aggfnoid == F_COUNT_ || a->aggfnoid == F_COUNT_ANY)
	{
		*f = NULL;
		if (a->aggstar)
			return AGG_COUNT;
		if (list_length(a->args) != 1)
			return AGG_NONE;
		arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
		r = reads(e->q->rtable, e->d, arg);
		if (r.bad || r.mine || contain_volatile_functions(arg))
			return AGG_NONE;
		*f = arg;
		return AGG_COUNT;
	}
	if (a->aggfnoid != F_SUM_NUMERIC || list_length(a->args) != 1)
		return AGG_NONE;
	arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
	if (IsA(arg, OpExpr))
	{
		OpExpr	   *m = (OpExpr *) arg;

		set_opfuncid(m);
		if (m->opfuncid == F_NUMERIC_MUL && list_length(m->args) == 2)
		{
			Reads		l = reads(e->q->rtable, e->d, linitial(m->args));

			r = reads(e->q->rtable, e->d, lsecond(m->args));
			if (!l.bad && !r.bad && !contain_volatile_functions(arg))
			{
				if (r.mine && !r.other && !l.mine && l.other)
					*dright = true;
				else if (l.mine && !l.other && !r.mine && r.other)
					*dright = false;
				else
					goto alone;
				*f = *dright ? linitial(m->args) : lsecond(m->args);
				*dx = *dright ? lsecond(m->args) : linitial(m->args);
				return exprType(*f) == NUMERICOID ? AGG_PRODUCT : AGG_NONE;
			}
		}
	}
alone:
	r = reads(e->q->rtable, e->d, arg);
	if (r.bad || r.mine || contain_volatile_functions(arg) || exprType(arg) != NUMERICOID)
		return AGG_NONE;
	*f = arg;
	return AGG_SUM;
}

/* `d.col = f.col` of the default equality of the type, f another entry of the FROM list: the two columns, d's first */
static bool
lookup_equality(Eager *e, Node *c, Var **dv, Var **fv)
{
	OpExpr	   *op;
	Node	   *l,
			   *r;

	if (!IsA(c, OpExpr) || list_length((op = (OpExpr *) c)->args) != 2)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (!IsA(l, Var) || !IsA(r, Var))
		return false;
	if (((Var *) r)->varno == e->d)
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (((Var *) l)->varno != e->d || ((Var *) r)->varno == e->d || ((Var *) l)->varlevelsup != 0 || ((Var *) r)->varlevelsup != 0 || ((Var *) l)->varattno <= 0 ||
		((Var *) r)->varattno <= 0 || ((Var *) l)->vartype != ((Var *) r)->vartype || !list_member_int(e->rtis, ((Var *) r)->varno) ||
		lookup_type_cache(((Var *) l)->vartype, TYPECACHE_EQ_OPR)->eq_opr != op->opno)
		return false;
	*dv = (Var *) l;
	*fv = (Var *) r;
	return true;
}

/* does the expression read a column outside the sums this rewrite moves and the expressions the query groups by? */
static bool
outside_walker(Node *n, Eager *e)
{
	Node	   *f = NULL,
			   *dx = NULL;
	bool		dright = false;

	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		return classify(e, (Aggref *) n, &f, &dx, &dright) == AGG_NONE;
	foreach_ptr(Node, g, e->gexprs)
		if (equal(n, g))
			return false;
	if (IsA(n, Var) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar))
		return true;
	return expression_tree_walker(n, outside_walker, e);
}

/* a sum of a product: the cheap test that the statements that cannot be moved fail first */
static bool
product_sum_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		return ((Aggref *) n)->aggfnoid == F_SUM_NUMERIC && list_length(((Aggref *) n)->args) == 1 && IsA(linitial_node(TargetEntry, ((Aggref *) n)->args)->expr, OpExpr);
	return expression_tree_walker(n, product_sum_walker, context);
}

/* the position of the aggregate among the ones the sub-select computes, which it is added to if it is new */
static int
part_index(Eager *e, Aggref *a, int kind, Node *f)
{
	int			i = 0;
	Part	   *p;

	foreach_ptr(Part, x, e->parts)
	{
		if (x->count == (kind == AGG_COUNT) && ((x->expr == NULL && f == NULL) || (x->expr != NULL && f != NULL && equal(x->expr, f))))
			return i;
		i++;
	}
	p = palloc(sizeof(Part));
	p->proto = a;
	p->count = kind == AGG_COUNT;
	p->expr = f;
	e->parts = lappend(e->parts, p);
	return i;
}

/* the aggregates of the query, each part once; true if one is not an aggregate this rewrite moves */
static bool
collect_walker(Node *n, Eager *e)
{
	Node	   *f = NULL,
			   *dx = NULL;
	bool		dright = false;
	int			kind;

	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
	{
		if ((kind = classify(e, (Aggref *) n, &f, &dx, &dright)) == AGG_NONE)
			return true;
		part_index(e, (Aggref *) n, kind, f);
		e->nproducts += kind == AGG_PRODUCT;
		e->counts |= kind == AGG_COUNT;
		return false;
	}
	return expression_tree_walker(n, collect_walker, e);
}

/* the table d is looked up by a unique key and read by the sums of products only; every other table is read by the groups, the sums' factors and the conditions */
static bool
try_lookup(Eager *e, List *quals, Index d)
{
	Query	   *q = e->q;
	RangeTblEntry *drte = rt_fetch(d, q->rtable);
	Var		   *dv = NULL;

	if (drte->relkind != RELKIND_RELATION || drte->tablesample != NULL || (drte->inh && has_subclass(drte->relid)))
		return false;
	e->d = d;
	e->join = NULL;
	e->rest = NIL;
	e->parts = NIL;
	e->nproducts = 0;
	e->counts = false;
	foreach_ptr(Node, cond, quals)
	{
		Reads		r = reads(q->rtable, d, cond);

		if (r.bad)
			return false;
		if (!r.mine)
		{
			/* a column compared with a constant picks rows: the normal plan reads those through an index, and planning this one too is paid by every statement of the shape */
			if (flat_picks_rows(cond))
				return false;
			e->rest = lappend(e->rest, cond);
		}
		else if (e->join == NULL && lookup_equality(e, cond, &dv, &e->fv))
			e->join = cond;
		else
			return false;
	}
	if (e->join == NULL || !sift_agg_unique_key(drte->relid, 1, 0, &(int){dv->varattno}, NULL))
		return false;
	foreach_ptr(Node, g, e->gexprs)
		if (reads(q->rtable, d, g).mine)
			return false;
	foreach_node(TargetEntry, te, q->targetList)
		if (collect_walker((Node *) te->expr, e) || outside_walker((Node *) te->expr, e))
			return false;
	/* a count of no rows is 0 where the sum of the counts of no groups is NULL: the query has its groups */
	return !collect_walker(q->havingQual, e) && !outside_walker(q->havingQual, e) && e->nproducts > 0 && (!e->counts || q->groupClause != NIL);
}

/* the query is one this rewrite can move: inner joins only, one table of them looked up by a unique key and read by sums of products */
static bool
analyze(Eager *e)
{
	Query	   *q = e->q;
	List	   *quals = NIL;
	int			nrels = 0;

	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind != RTE_JOIN)
			nrels += rte->rtekind == RTE_RELATION ? 1 : 1000;
	flat_inner_items((Node *) q->jointree, &e->rtis, &quals);
	if (nrels != list_length(e->rtis) || nrels < 2)
		return false;
	foreach_node(SortGroupClause, sgc, q->groupClause)
	{
		TargetEntry *te = get_sortgroupclause_tle(sgc, q->targetList);

		if (reads(q->rtable, 0, (Node *) te->expr).bad || contain_volatile_functions((Node *) te->expr))
			return false;
		e->gexprs = lappend(e->gexprs, te->expr);
	}
	foreach_int(d, e->rtis)
		if (try_lookup(e, quals, d))
			return true;
	return false;
}

static RangeTblRef *
rtref(int rti)
{
	RangeTblRef *r = makeNode(RangeTblRef);

	r->rtindex = rti;
	return r;
}

/* the query above the sub-select: its group expressions and sums are columns of it, and a sum is the sum of the products of the sub-select's sums by the lookup's value */
static Node *
outer_mutator(Node *n, Eager *e)
{
	Node	   *f = NULL,
			   *dx = NULL;
	bool		dright = false;
	int			i = 0;

	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		Aggref	   *a = copyObject((Aggref *) n);
		int			kind = classify(e, (Aggref *) n, &f, &dx, &dright),
					col = list_length(e->gexprs) + 2 + part_index(e, (Aggref *) n, kind, f);
		TargetEntry *te;

		if (kind == AGG_COUNT)
		{
			/* the sum of the counts of the groups, as the bigint the count was */
			a->aggfnoid = F_SUM_INT8;
			a->aggtype = NUMERICOID;
			a->aggstar = false;
			a->aggargtypes = list_make1_oid(INT8OID);
			a->args = list_make1(makeTargetEntry((Expr *) makeVar(1, col, INT8OID, -1, InvalidOid, 0), 1, NULL, false));
			return (Node *) makeFuncExpr(F_INT8_NUMERIC, INT8OID, list_make1(a), InvalidOid, InvalidOid, COERCE_EXPLICIT_CAST);
		}
		te = linitial_node(TargetEntry, a->args);	/* a count(*) has no argument */
		if (kind == AGG_SUM)
			te->expr = (Expr *) makeVar(1, col, NUMERICOID, -1, InvalidOid, 0);
		else
		{
			OpExpr	   *m = (OpExpr *) te->expr;
			Node	   *d2 = copyObject(dx);
			Var		   *s = makeVar(1, col, NUMERICOID, -1, InvalidOid, 0);

			ChangeVarNodes(d2, e->d, 2, 0);
			m->args = dright ? list_make2(s, d2) : list_make2(d2, s);
		}
		return (Node *) a;
	}
	foreach_ptr(Node, g, e->gexprs)
	{
		i++;
		if (equal(n, g))
			return (Node *) makeVar(1, i, exprType(g), exprTypmod(g), exprCollation(g), 0);
	}
	return expression_tree_mutator(n, outer_mutator, e);
}

/* the join of the sub-select to the lookup: the lookup's columns are the entry 2, the key of the other table is the sub-select's column after the groups */
static Node *
join_mutator(Node *n, Eager *e)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varno == e->fv->varno && ((Var *) n)->varattno == e->fv->varattno)
		return (Node *) makeVar(1, list_length(e->gexprs) + 1, e->fv->vartype, e->fv->vartypmod, e->fv->varcollid, 0);
	if (IsA(n, Var) && ((Var *) n)->varno == e->d)
	{
		Var		   *v = copyObject((Var *) n);

		v->varno = v->varnosyn = 2;
		return (Node *) v;
	}
	return expression_tree_mutator(n, join_mutator, e);
}

/* the sub-select: the tables but the lookup, grouped by the keys of the query and the column the lookup is joined by, with the sum of each factor */
static Query *
inner_query(Eager *e)
{
	Query	   *q = e->q,
			   *s = makeNode(Query);
	List	   *froms = NIL,
			   *tlist = NIL;
	TargetEntry *key;
	int			resno = 1;

	s->commandType = CMD_SELECT;
	s->querySource = QSRC_ORIGINAL;
	s->canSetTag = true;
	s->rtable = copyObject(q->rtable);
	s->rteperminfos = copyObject(q->rteperminfos);
	foreach_int(rti, e->rtis)
		if (rti != e->d)
			froms = lappend(froms, rtref(rti));
	s->jointree = makeFromExpr(froms, e->rest == NIL ? NULL : list_length(e->rest) == 1 ? copyObject(linitial(e->rest)) : (Node *) makeBoolExpr(AND_EXPR, copyObject(e->rest), -1));
	foreach_node(SortGroupClause, sgc, q->groupClause)
	{
		TargetEntry *te = get_sortgroupclause_tle(sgc, q->targetList),
				   *n = makeTargetEntry((Expr *) copyObject(te->expr), resno, psprintf("g%d", resno), false);

		resno++;
		n->ressortgroupref = te->ressortgroupref;
		tlist = lappend(tlist, n);
		s->groupClause = lappend(s->groupClause, copyObject(sgc));
	}
	key = makeTargetEntry((Expr *) copyObject(e->fv), resno++, "k", false);
	s->targetList = lappend(tlist, key);
	group_by(s, key);
	foreach_ptr(Part, part, e->parts)
	{
		Aggref	   *sum = copyObject(part->proto);

		if (part->expr != NULL)
			linitial_node(TargetEntry, sum->args)->expr = (Expr *) copyObject(part->expr);
		s->targetList = lappend(s->targetList, makeTargetEntry((Expr *) sum, resno, psprintf("s%d", resno - list_length(e->gexprs) - 1), false));
		resno++;
	}
	s->hasAggs = true;
	return s;
}

/* could the statement be one: a grouped select of joined tables with a sum of a product (the test that costs no copy of the query) */
bool
sift_agg_eager_candidate(Query *q)
{
	return q->commandType == CMD_SELECT && q->hasAggs && !q->hasSubLinks && !q->hasWindowFuncs && !q->hasTargetSRFs && q->groupingSets == NIL && q->distinctClause == NIL &&
		q->setOperations == NULL && q->cteList == NIL && q->rowMarks == NIL && q->jointree != NULL && list_length(q->rtable) >= 2 &&
		(product_sum_walker((Node *) q->targetList, NULL) || product_sum_walker(q->havingQual, NULL));
}

bool
sift_agg_eager_lookup(Query *q, bool apply)
{
	Eager		e = {0};
	Query	   *s;
	RangeTblEntry *arte,
			   *drte;
	List	   *names = NIL;

	if (!sift_agg_eager_candidate(q))
		return false;
	e.q = copyObject(q);
	flat_ungroup(e.q);
	if (!analyze(&e))
		return false;
	if (!apply)
		return true;
	s = inner_query(&e);
	e.q->targetList = (List *) outer_mutator((Node *) e.q->targetList, &e);
	e.q->havingQual = outer_mutator(e.q->havingQual, &e);
	foreach_node(TargetEntry, te, s->targetList)
		names = lappend(names, makeString(te->resname));
	arte = makeNode(RangeTblEntry);
	arte->rtekind = RTE_SUBQUERY;
	arte->subquery = s;
	arte->inFromCl = true;
	arte->eref = makeAlias("pre", names);
	drte = copyObject(rt_fetch(e.d, e.q->rtable));
	e.q->rteperminfos = list_make1(copyObject(getRTEPermissionInfo(e.q->rteperminfos, drte)));
	drte->perminfoindex = 1;
	e.q->jointree = makeFromExpr(list_make2(rtref(1), rtref(2)), join_mutator(e.join, &e));
	e.q->rtable = list_make2(arte, drte);
	*q = *e.q;
	sift_agg_by_cost = sift_agg_flat_any = true;
	return true;
}
