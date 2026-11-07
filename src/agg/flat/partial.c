/*
 * agg/flat/partial.c - the aggregates of a table left-joined to a join of small tables are made for each key first
 * `SELECT k.name, r.region, count(e.id) FROM kinds k CROSS JOIN regions r LEFT JOIN events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2` has a row for every kind
 * and region, with its count, zero for the pairs of no event. Two tables are not the one table of a preserved lookup, so the scan stayed out of it and the join read every event.
 * The events of a pair are the events of one group of `events GROUP BY kind_id, region`, so the sub-select makes the groups and the query joins them to the pairs: the join gives a
 * pair its group, or NULL; a count is the group's (0 for the pairs of none), a sum, min or max is the sum, min or max of the group's (NULL for none), and count(*) counts the
 * row a pair of no event keeps as 1. Only the aggregates that add up from groups of any partition (pushagg.c) and a key that is a column of the table equated by the default
 * equality with an expression of the other tables. The planner hook plans the query both ways and keeps this one if it runs in the scan and costs less.
 */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "utils/fmgroids.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/clauses.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

typedef struct Partial
{
	Query	   *q;				/* the query as PostgreSQL 17 parses it (a copy: the rewrite changes it) */
	Index		e;				/* the entry of the table */
	JoinExpr   *join;			/* the left join of the other tables to it */
	List	   *keys;			/* the columns of the table the join equates (Var) */
	List	   *others;			/* what each equals */
	List	   *inner;			/* the conditions over the table alone */
	List	   *outer;			/* the conditions over the other tables alone */
	List	   *aggs;			/* the aggregates of the query, each once */
	Bitmapset  *nulls;			/* the join that makes the sub-select's columns NULL for a pair of no group */
} Partial;

/* an expression the rewrite can move: nothing volatile, no sub-select, no aggregate, no column of another level */
static bool
plain(Node *n)
{
	return !checkExprHasSubLink(n) && !contain_volatile_functions(n) && !contain_agg_clause(n) && !contain_window_function(n) && !contain_vars_of_level(n, 1);
}

static bool
reads_walker(Node *n, Bitmapset **rels)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		if (((Var *) n)->varlevelsup == 0)
			*rels = bms_add_member(*rels, ((Var *) n)->varno);
		return false;
	}
	return expression_tree_walker(n, reads_walker, rels);
}

/* the entries of this level that the expression reads (not the joins that may make a column NULL, which pull_varnos adds) */
static Bitmapset *
reads(Node *n)
{
	Bitmapset  *rels = NULL;

	reads_walker(n, &rels);
	return rels;
}

/* `e.col = expr` of the default equality of the column's type, expr reading no column of e */
static bool
key_equality(Partial *p, Node *c, Var **col, Node **other)
{
	OpExpr	   *op;
	Node	   *sides[2];

	if (!IsA(c, OpExpr) || list_length((op = (OpExpr *) c)->args) != 2)
		return false;
	sides[0] = strip_relabel(linitial(op->args));
	sides[1] = strip_relabel(lsecond(op->args));
	for (int i = 0; i < 2; i++)
	{
		Var		   *v;

		if (!IsA(sides[i], Var))
			continue;
		v = (Var *) sides[i];
		if (v->varno != p->e || v->varlevelsup != 0 || v->varattno <= 0 || bms_is_member(p->e, reads(sides[1 - i])) || exprType(sides[1 - i]) != v->vartype ||
			lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr != op->opno)
			continue;
		*col = v;
		*other = sides[1 - i];
		return true;
	}
	return false;
}

/* is a column of the table, or of a join, read outside the aggregates? (the aggregates are replaced, and the join's columns are the table's) */
static bool
outside_walker(Node *n, Partial *p)
{
	if (n == NULL || IsA(n, Aggref))
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup == 0 && (((Var *) n)->varno == p->e || rt_fetch(((Var *) n)->varno, p->q->rtable)->rtekind == RTE_JOIN);
	return expression_tree_walker(n, outside_walker, p);
}

/* the aggregates of the query, each once; true if one cannot be added up from the groups of the table */
static bool
collect_walker(Node *n, Partial *p)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;
		Bitmapset  *r = reads((Node *) a->args);

		if (!flat_addable(a) || !plain((Node *) a->args) || (!bms_is_empty(r) && !bms_equal(r, bms_make_singleton(p->e))))
			return true;
		if (!list_member(p->aggs, a))
			p->aggs = lappend(p->aggs, a);
		return false;
	}
	return expression_tree_walker(n, collect_walker, p);
}

/* the query is one this rewrite can move: one left join of a join of tables to a table with a column index, nothing else in the FROM list */
static bool
analyze(Partial *p)
{
	Query	   *q = p->q;
	RangeTblEntry *rte;
	List	   *conds;

	if (list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), JoinExpr))
		return false;
	p->join = linitial_node(JoinExpr, q->jointree->fromlist);
	if (p->join->jointype != JOIN_LEFT || !IsA(p->join->larg, JoinExpr) || !IsA(p->join->rarg, RangeTblRef) || p->join->quals == NULL)
		return false;
	p->e = ((RangeTblRef *) p->join->rarg)->rtindex;
	rte = rt_fetch(p->e, q->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->tablesample != NULL || rte->lateral || !has_column_source(rte->relid))
		return false;
	if (q->jointree->quals != NULL && bms_is_member(p->e, reads(q->jointree->quals)))
		return false;
	conds = make_ands_implicit((Expr *) p->join->quals);
	foreach_ptr(Node, c, conds)
	{
		Var		   *col;
		Node	   *other;
		Bitmapset  *r = reads(c);

		if (!plain(c))
			return false;
		if (!bms_is_member(p->e, r))
			p->outer = lappend(p->outer, c);
		else if (bms_membership(r) == BMS_SINGLETON)
			p->inner = lappend(p->inner, c);
		else if (key_equality(p, c, &col, &other))
		{
			p->keys = lappend(p->keys, col);
			p->others = lappend(p->others, other);
		}
		else
			return false;
	}
	if (p->keys == NIL)
		return false;
	foreach_node(TargetEntry, te, q->targetList)
		if (collect_walker((Node *) te->expr, p) || outside_walker((Node *) te->expr, p))
			return false;
	return !collect_walker(q->havingQual, p) && !outside_walker(q->havingQual, p) && !outside_walker(q->jointree->quals, p) && p->aggs != NIL;
}

/* could the statement be one: a grouped select of a left join of a join, to a table with a column index (the test that costs no copy of the query) */
bool
sift_agg_partial_candidate(Query *q)
{
	JoinExpr   *j;

	if (q->commandType != CMD_SELECT || !q->hasAggs || q->hasSubLinks || q->hasWindowFuncs || q->hasTargetSRFs || q->groupingSets != NIL || q->distinctClause != NIL ||
		q->setOperations != NULL || q->cteList != NIL || q->rowMarks != NIL || q->jointree == NULL || list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), JoinExpr))
		return false;
	j = linitial_node(JoinExpr, q->jointree->fromlist);
	return j->jointype == JOIN_LEFT && IsA(j->larg, JoinExpr) && IsA(j->rarg, RangeTblRef) &&
		rt_fetch(((RangeTblRef *) j->rarg)->rtindex, q->rtable)->rtekind == RTE_RELATION;
}

static Node *
count_of(Node *e, int64 empty)
{
	CoalesceExpr *c = makeNode(CoalesceExpr);

	c->coalescetype = INT8OID;
	c->args = list_make2(e, makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(empty), false, FLOAT8PASSBYVAL));
	c->location = -1;
	return (Node *) c;
}

/* the query above the sub-select: an aggregate adds up the column the sub-select has for it (a count is 0 where no group is there, and count(*) counts the row of a key of no group once) */
static Node *
outer_mutator(Node *n, Partial *p)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;
		int			i = list_length(p->keys) + 1;
		Node	   *col;

		foreach_ptr(Aggref, x, p->aggs)
		{
			if (equal(x, a))
				break;
			i++;
		}
		col = (Node *) makeVar(p->e, i, a->aggtype, -1, a->aggcollid, 0);
		((Var *) col)->varnullingrels = p->nulls;
		if (a->aggfnoid == F_COUNT_)
			return count_of(flat_add_up(a, count_of(col, 1)), 0);
		if (a->aggfnoid == F_COUNT_ANY)
			return count_of(flat_add_up(a, col), 0);
		return flat_add_up(a, col);
	}
	return expression_tree_mutator(n, outer_mutator, p);
}

/* a column the sub-select reads is the table's own, not one a join above makes NULL */
static bool
unnull_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		((Var *) n)->varnullingrels = NULL;
		return false;
	}
	return expression_tree_walker(n, unnull_walker, context);
}

/* the sub-select: the table's rows that the conditions over it alone keep, grouped by the columns the join equates, with the aggregates of the query */
static Query *
inner_query(Partial *p)
{
	RangeTblEntry *erte = rt_fetch(p->e, p->q->rtable);
	Query	   *s = makeNode(Query);
	RangeTblRef *ref = makeNode(RangeTblRef);
	Node	   *where = NULL;
	int			resno = 1;

	s->commandType = CMD_SELECT;
	s->querySource = QSRC_ORIGINAL;
	s->canSetTag = true;
	s->rtable = list_make1(copyObject(erte));
	s->rteperminfos = list_make1(copyObject(getRTEPermissionInfo(p->q->rteperminfos, erte)));
	linitial_node(RangeTblEntry, s->rtable)->perminfoindex = 1;
	ref->rtindex = 1;
	if (p->inner != NIL)
	{
		where = (Node *) make_ands_explicit(copyObject(p->inner));
		ChangeVarNodes(where, p->e, 1, 0);
		unnull_walker(where, NULL);
	}
	s->jointree = makeFromExpr(list_make1(ref), where);
	foreach_ptr(Var, k, p->keys)
	{
		TargetEntry *te = makeTargetEntry((Expr *) copyObject(k), resno, psprintf("g%d", resno), false);

		((Var *) te->expr)->varno = ((Var *) te->expr)->varnosyn = 1;
		s->targetList = lappend(s->targetList, te);
		group_by(s, te);
		resno++;
	}
	foreach_ptr(Aggref, a, p->aggs)
	{
		Aggref	   *c = copyObject(a);

		ChangeVarNodes((Node *) c, p->e, 1, 0);
		unnull_walker((Node *) c, NULL);
		s->targetList = lappend(s->targetList, makeTargetEntry((Expr *) c, resno, psprintf("a%d", resno - list_length(p->keys)), false));
		resno++;
	}
	s->hasAggs = true;
	return s;
}

bool
sift_agg_partial_left(Query *q, bool apply)
{
	Partial		p = {0};
	Query	   *s;
	JoinExpr   *jx;
	RangeTblEntry *erte,
			   *arte = makeNode(RangeTblEntry);
	List	   *names = NIL,
			   *on = NIL;
	int			i = 1;

	if (!sift_agg_partial_candidate(q))
		return false;
	p.q = copyObject(q);
	flat_ungroup(p.q);
	if (!analyze(&p))
		return false;
	if (!apply)
		return true;
	s = inner_query(&p);
	foreach_node(TargetEntry, te, s->targetList)
		names = lappend(names, makeString(te->resname));
	erte = rt_fetch(p.e, p.q->rtable);
	arte->rtekind = RTE_SUBQUERY;
	arte->subquery = s;
	arte->inFromCl = true;
	arte->alias = erte->alias;
	arte->eref = makeAlias(erte->eref->aliasname, names);
	lfirst(list_nth_cell(p.q->rtable, p.e - 1)) = arte;
	foreach_ptr(Var, k, p.keys)
	{
		OpExpr	   *eq = (OpExpr *) make_opclause(lookup_type_cache(k->vartype, TYPECACHE_EQ_OPR)->eq_opr, BOOLOID, false,
												  (Expr *) makeVar(p.e, i, k->vartype, k->vartypmod, k->varcollid, 0), (Expr *) list_nth(p.others, i - 1), InvalidOid, k->varcollid);

		set_opfuncid(eq);
		on = lappend(on, eq);
		i++;
	}
	on = list_concat(on, p.outer);
	jx = flat_join_item(p.q, p.join->larg, p.e, JOIN_LEFT, (Node *) make_ands_explicit(on));
	p.q->jointree->fromlist = list_make1(jx);
	p.nulls = bms_make_singleton(jx->rtindex);
	p.q->targetList = (List *) outer_mutator((Node *) p.q->targetList, &p);
	p.q->havingQual = outer_mutator(p.q->havingQual, &p);
	*q = *p.q;
	sift_agg_by_cost = sift_agg_flat_any = true;
	return true;
}
