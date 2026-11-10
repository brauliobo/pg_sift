/*
 * agg/flat/keypush.c - the keys of a filtered table joined to a grouped sub-select are looked up before the groups are made
 * `FROM (SELECT k, count(*) FROM f GROUP BY k) a JOIN d ON d.id = a.k WHERE d.name = 'x'` makes every group of f and keeps the one that d has a row for. The groups of
 * the keys d keeps are the same groups (every row of a group has its key), so the sub-select can ask for them: `WHERE k IN (SELECT id FROM d WHERE d.name = 'x')` is
 * a semi join that the planner reads as a few probes of an index of f, where the groups of the whole table cost a scan. The join above stays as it is. Only a join
 * of the inner kind keeps a row for a group that d has no row for; a sub-select that cuts its groups itself (a LIMIT, a window function, DISTINCT) is not one whose
 * groups can be chosen first. The planner hook plans the query both ways and takes this one where its cost is clearly the lower, with the scan or without it.
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

bool		sift_agg_flat_any;
bool		sift_agg_flat_in_scan;
double		sift_agg_flat_margin;

/* the entries of the inner joins of a level (their ON conditions and its WHERE are conditions of the same rows) and the conditions */
void
flat_inner_items(Node *n, List **rtis, List **quals)
{
	if (n == NULL)
		return;
	if (IsA(n, RangeTblRef))
		*rtis = lappend_int(*rtis, ((RangeTblRef *) n)->rtindex);
	else if (IsA(n, FromExpr))
	{
		FromExpr   *f = (FromExpr *) n;

		if (f->quals != NULL)
			*quals = list_concat(*quals, make_ands_implicit((Expr *) f->quals));
		foreach_ptr(Node, x, f->fromlist)
			flat_inner_items(x, rtis, quals);
	}
	else if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER)
	{
		JoinExpr   *j = (JoinExpr *) n;

		if (j->quals != NULL)
			*quals = list_concat(*quals, make_ands_implicit((Expr *) j->quals));
		flat_inner_items(j->larg, rtis, quals);
		flat_inner_items(j->rarg, rtis, quals);
	}
}

/* a sub-select whose groups are the rows it returns, each with the key of its group; one with a sub-select of its own in a condition is left to what it asks already */
static bool
groups_only(Query *sub)
{
	return sub->commandType == CMD_SELECT && sub->setOperations == NULL && sub->groupClause != NIL && sub->groupingSets == NIL && !sub->hasWindowFuncs &&
		!sub->hasSubLinks && sub->limitCount == NULL && sub->limitOffset == NULL && sub->distinctClause == NIL && !sub->hasTargetSRFs && sub->rowMarks == NIL && sub->jointree != NULL;
}

/* the expression the sub-select groups by that its output column attno is, else NULL */
static Node *
group_key_of(Query *sub, AttrNumber attno)
{
	TargetEntry *te;
	Node	   *e;

	if (attno < 1 || attno > list_length(sub->targetList))
		return NULL;
	te = list_nth_node(TargetEntry, sub->targetList, attno - 1);
	e = (Node *) te->expr;
#if PG_VERSION_NUM >= 180000
	/* from PostgreSQL 18 the grouped query reads its keys as columns of the entry of its grouping step */
	if (IsA(e, Var) && ((Var *) e)->varlevelsup == 0 && rt_fetch(((Var *) e)->varno, sub->rtable)->rtekind == RTE_GROUP)
		e = list_nth(rt_fetch(((Var *) e)->varno, sub->rtable)->groupexprs, ((Var *) e)->varattno - 1);
#endif
	if (te->resjunk || te->ressortgroupref == 0 || checkExprHasSubLink(e) || contain_volatile_functions(e))
		return NULL;
	foreach_node(SortGroupClause, sgc, sub->groupClause)
		if (sgc->tleSortGroupRef == te->ressortgroupref)
			return e;
	return NULL;
}

/* `d.col = a.col` of the default equality of the type, d and a the entries: the two variables, d's first */
static bool
join_equality(Node *c, Index d, Index a, Var **dv, Var **av)
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
	if (((Var *) l)->varno == a && ((Var *) r)->varno == d)
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (((Var *) l)->varno != d || ((Var *) r)->varno != a || ((Var *) l)->varlevelsup != 0 || ((Var *) r)->varlevelsup != 0 || ((Var *) l)->varattno <= 0 ||
		((Var *) r)->varattno <= 0 || ((Var *) l)->vartype != ((Var *) r)->vartype ||
		lookup_type_cache(((Var *) l)->vartype, TYPECACHE_EQ_OPR)->eq_opr != op->opno)
		return false;
	*dv = (Var *) l;
	*av = (Var *) r;
	return true;
}

/* `key = ANY (SELECT d.col FROM d WHERE dconds)`, the sublink a parsed IN makes */
static SubLink *
key_sublink(Query *q, RangeTblEntry *drte, Index d, Var *dv, Node *key, OpExpr *eq, List *dconds)
{
	Query	   *k = makeNode(Query);
	SubLink    *sl = makeNode(SubLink);
	OpExpr	   *op = copyObject(eq);
	Param	   *p = makeNode(Param);
	RangeTblEntry *rte = copyObject(drte);
	Var		   *out = copyObject(dv);
	List	   *quals = copyObject(dconds);
	RTEPermissionInfo *perm = getRTEPermissionInfo(q->rteperminfos, drte);

	/* the table is entry 1 of its own sub-select, with its permission entry */
	ChangeVarNodes((Node *) quals, d, 1, 0);
	out->varno = 1;
	out->varnosyn = 1;
	rte->perminfoindex = 1;
	k->commandType = CMD_SELECT;
	k->querySource = QSRC_ORIGINAL;
	k->canSetTag = true;
	k->rtable = list_make1(rte);
	k->rteperminfos = list_make1(copyObject(perm));
	k->jointree = makeFromExpr(list_make1(makeNode(RangeTblRef)), list_length(quals) == 1 ? linitial(quals) : (Node *) makeBoolExpr(AND_EXPR, quals, -1));
	linitial_node(RangeTblRef, k->jointree->fromlist)->rtindex = 1;
	k->targetList = list_make1(makeTargetEntry((Expr *) out, 1, "k", false));
	p->paramkind = PARAM_SUBLINK;
	p->paramid = 1;
	p->paramtype = dv->vartype;
	p->paramtypmod = dv->vartypmod;
	p->paramcollid = dv->varcollid;
	p->location = -1;
	/* the sides in the order the join wrote them */
	if (IsA(strip_relabel(linitial(op->args)), Var) && ((Var *) strip_relabel(linitial(op->args)))->varno == d)
		op->args = list_make2(p, copyObject(key));
	else
		op->args = list_make2(copyObject(key), p);
	sl->subLinkType = ANY_SUBLINK;
	sl->subLinkId = 0;
	sl->testexpr = (Node *) op;
	sl->operName = list_make1(makeString("="));
	sl->subselect = (Node *) k;
	sl->location = -1;
	return sl;
}

/* a constant of the statement: a literal, a parameter, a list of them */
static bool
fixed_operand(Node *n)
{
	n = strip_relabel(n);
	if (IsA(n, ArrayExpr))
	{
		foreach_ptr(Node, e, ((ArrayExpr *) n)->elements)
			if (!fixed_operand(e))
				return false;
		return true;
	}
	return IsA(n, Const) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXTERN);
}

/* a condition that picks rows by comparing a column with a constant `col op const` (a list of keys or a range is what the key lookup is for) */
bool
flat_picks_rows(Node *c)
{
	List	   *args = IsA(c, OpExpr) ? ((OpExpr *) c)->args : IsA(c, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) c)->args : NIL;

	if (list_length(args) != 2)
		return false;
	return (IsA(strip_relabel(linitial(args)), Var) && fixed_operand(lsecond(args))) || (IsA(strip_relabel(lsecond(args)), Var) && fixed_operand(linitial(args)));
}

/* the conditions of `d` alone: no other entry, no sub-select, nothing volatile */
static List *
alone_conds(List *quals, Index d, Node *join)
{
	List	   *r = NIL;

	foreach_ptr(Node, c, quals)
	{
		Bitmapset  *rels;

		if (c == join || checkExprHasSubLink(c) || contain_volatile_functions(c) || contain_vars_of_level(c, 1))
			continue;
		rels = pull_varnos_of_level(NULL, c, 0);
		if (bms_membership(rels) == BMS_SINGLETON && bms_is_member(d, rels))
			r = lappend(r, c);
	}
	return r;
}

static bool
picks_any(List *conds)
{
	foreach_ptr(Node, c, conds)
		if (flat_picks_rows(c))
			return true;
	return false;
}

/* the sub-select holds this lookup of keys already, as a condition of its own */
static bool
looked_up(Query *sub, SubLink *sl)
{
	Node	   *quals = sub->jointree->quals;

	if (quals == NULL)
		return false;
	return equal(quals, sl) || (IsA(quals, BoolExpr) && list_member(((BoolExpr *) quals)->args, sl));
}

bool
sift_agg_push_dim_keys(Query *q, bool apply)
{
	List	   *rtis = NIL,
			   *quals = NIL;
	bool		changed = false;

	if (q->commandType != CMD_SELECT || q->jointree == NULL)
		return false;
	flat_inner_items(q->jointree->quals != NULL || q->jointree->fromlist != NIL ? (Node *) q->jointree : NULL, &rtis, &quals);
	foreach_int(a, rtis)
	{
		RangeTblEntry *arte = rt_fetch(a, q->rtable);
		Query	   *sub;

		if (arte->rtekind != RTE_SUBQUERY || arte->lateral || !groups_only(sub = arte->subquery))
			continue;
		foreach_int(d, rtis)
		{
			RangeTblEntry *drte = rt_fetch(d, q->rtable);
			Var		   *dv,
					   *av;
			Node	   *join = NULL,
					   *key = NULL;
			List	   *dconds;
			SubLink    *sl;

			if (d == a || drte->rtekind != RTE_RELATION || (drte->relkind != RELKIND_RELATION && drte->relkind != RELKIND_PARTITIONED_TABLE) || drte->tablesample != NULL)
				continue;
			foreach_ptr(Node, c, quals)
				if (join_equality(c, d, a, &dv, &av) && (key = group_key_of(sub, av->varattno)) != NULL)
				{
					join = c;
					break;
				}
					if (join == NULL)
				continue;
			/* a table of keys: a row for each key, and a condition that picks few of them. A table that repeats its key (a fact table) is not what the lookup asks for, and planning the statement
			 * a second time for it would be paid by every statement of that shape */
			dconds = alone_conds(quals, d, join);
			if (dconds == NIL || !sift_agg_unique_key(drte->relid, 1, 0, &(int){dv->varattno}, NULL) || !picks_any(dconds))
				continue;
			sl = key_sublink(q, drte, d, dv, key, (OpExpr *) join, dconds);
			/* the flattening runs this step twice (before the probes, and after the sub-selects the other rewrites make): the keys are looked up once */
			if (looked_up(sub, sl))
				continue;
			changed = true;
			if (apply)
			{
				sub->jointree->quals = sub->jointree->quals == NULL ? (Node *) sl : (Node *) makeBoolExpr(AND_EXPR, list_make2(sub->jointree->quals, sl), -1);
				sub->hasSubLinks = true;
			}
		}
	}
	if (changed && apply)
	{
		sift_agg_by_cost = true;
		sift_agg_flat_any = true;
	}
	return changed;
}
