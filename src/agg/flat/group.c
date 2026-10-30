/*
 * agg/flat/group.c - the aggregates of a lateral as a grouping of the outer table
 * The LATERAL sub-select of aggregates that has one row for each row of x, rewritten as the
 * grouping of its elements by the row of x.
 */

/*
 * `x LEFT JOIN LATERAL (SELECT avg(e) FROM f(x.doc) WHERE ..) s ON true` has one row for each row of x: the aggregates of its elements are
 * a column of x, and the scan runs the grouping of the elements by the row of x (`.. FROM x LEFT JOIN LATERAL f(x.doc) GROUP BY x.id`). The
 * rewrite is made where that is the same query: x holds each of its keys once, and an aggregate of the elements the lateral left out is
 * what it is of the one row the outer join makes up (NULL for a strict argument).
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
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

static List *unique_columns(Query *x, List *levels);

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

void
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
bool
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
