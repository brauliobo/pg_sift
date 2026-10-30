/*
 * agg/two/arms.c - the arms of a two level aggregate
 * The sub-selects or CTE bodies an outer aggregate reads, their private range-table
 * entries, and the conditions they share or split.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "catalog/pg_type.h"

#include "agg/two/int.h"


Node *
cte_sub_mutator(Node *n, CteSub *s)
{
	if (n == NULL)
		return NULL;
	if (s->rti != 0 && IsA(n, Var) && ((Var *) n)->varno == s->rti && ((Var *) n)->varlevelsup == 0)
	{
		TargetEntry *te = get_tle_by_resno(s->tlist, ((Var *) n)->varattno);

		return te ? (Node *) copyObject(te->expr) : n;
	}
	return expression_tree_mutator(n, cte_sub_mutator, s);
}

/* the root of the common table expression a CTE scan of `root` reads; NULL if it is not one the scan can look through */
PlannerInfo *
cte_root(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;
	CommonTableExpr *cte = NULL;

	for (Index up = rte->ctelevelsup; up > 0 && cteroot != NULL; up--)
		cteroot = cteroot->parent_root;
	if (cteroot == NULL)
		return NULL;
	foreach_node(CommonTableExpr, x, cteroot->parse->cteList)
	{
		if (strcmp(x->ctename, rte->ctename) == 0)
		{
			cte = x;
			break;
		}
		ndx++;
	}
	if (cte == NULL || cte->cterecursive || cte->search_clause || cte->cycle_clause || ndx >= list_length(cteroot->cte_plan_ids))
		return NULL;
	return list_nth(root->glob->subroots, list_nth_int(cteroot->cte_plan_ids, ndx) - 1);
}

/* the subqueries of a UNION ALL: a member that is itself a UNION ALL (a subquery of arms in an arm's place) gives its own */
static bool
append_arms(PlannerInfo *root, Index parent, List **arms)
{
	foreach_node(AppendRelInfo, ai, root->append_rel_list)
	{
		RelOptInfo *rel;
		int			i = 0;

		if (ai->parent_relid != parent)
			continue;
		rel = root->simple_rel_array[ai->child_relid];
		if (rel == NULL)
			return false;
		foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
			if (!rinfo->pseudoconstant)
				return false;
		foreach_ptr(Node, tv, ai->translated_vars)
			if (tv == NULL || !IsA(tv, Var) || ((Var *) tv)->varno != ai->child_relid || ((Var *) tv)->varattno != ++i)
				return false;
		if (rel->subroot != NULL)
			*arms = lappend(*arms, rel->subroot);
		else if (!rt_fetch(ai->child_relid, root->parse->rtable)->inh || !append_arms(root, ai->child_relid, arms))
			return false;
	}
	return true;
}

/*
 * The arms of the outer query's one FROM item: the subqueries of a UNION ALL (appendrel) or the subquery itself. The quals of the
 * outer query that PostgreSQL pushed into the arms (to their WHERE, or HAVING) are theirs now: an arm whose rel still has a qual
 * (one it did not push: volatile, a one-time filter) is not one.
 */
List *
outer_arms(PlannerInfo *root, Index srti, List **gates)
{
	List	   *arms = NIL;
	RelOptInfo *parent = root->simple_rel_array[srti];

	if (parent == NULL)
		return NIL;
	/* the conditions without a column stay above the arms as PostgreSQL's one-time filter: the scan takes them as its own gate */
	foreach_node(RestrictInfo, rinfo, parent->baserestrictinfo)
		if (rinfo->pseudoconstant)
			*gates = lappend(*gates, rinfo->clause);
		else if (!rt_fetch(srti, root->parse->rtable)->inh)
			return NIL;
	if (!rt_fetch(srti, root->parse->rtable)->inh)
		return parent->subroot ? list_make1(parent->subroot) : NIL;
	return append_arms(root, srti, &arms) ? arms : NIL;
}

/*
 * The table an arm reads: through its CTE scan to the common table expression's root, or the arm's own root (its FROM is the scan itself:
 * a table with its unnests and lookups, which is what an arm over a common table expression that PostgreSQL inlined looks like); NULL if
 * the arm is not a simple aggregate
 */
PlannerInfo *
arm_base(PlannerInfo *arm, ArmIn *in)
{
	Query	   *q = arm->parse;
	RangeTblEntry *rte;

	if (!q->hasAggs || q->groupClause == NIL || q->groupingSets || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->distinctClause || q->sortClause || q->limitCount || q->limitOffset || q->groupDistinct || q->hasForUpdate ||
		q->setOperations || q->cteList != NIL)
		return NULL;
	in->sub.rti = 0;
	if (list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return arm;
	rte = rt_fetch(((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex, q->rtable);
	if (rte->rtekind != RTE_CTE)
		return arm;
	in->sub.rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	return cte_root(arm, rte);
}

/*
 * Two arms that read their own copy of one scan (a common table expression that PostgreSQL inlined into each): the same tables and joins.
 * Their WHERE may differ (see TwoLevel.drop). The entries of pulled up subqueries and of the grouping belong to their arm, and the column
 * names a plan prunes differ: none counts. The join entries are the jointree's, which is compared as well.
 */
static bool
arm_private_rte(RTEKind kind)
{
#if PG_VERSION_NUM >= 180000
	if (kind == RTE_GROUP)
		return true;
#endif
	return kind == RTE_SUBQUERY || kind == RTE_JOIN;
}

/*
 * The conditions of a tree of joins that filter its tables as one WHERE does: those of the FROM lists and the inner joins, wherever they sit in the
 * tree (PostgreSQL leaves a condition at the join that makes it evaluable, which an outer join made inner changes), but not those of a nullable side.
 */
static List *
from_conds(Node *n, List *acc)
{
	if (n == NULL)
		return acc;
	if (IsA(n, List))
		foreach_ptr(Node, x, (List *) n)
			acc = from_conds(x, acc);
	else if (IsA(n, FromExpr))
	{
		acc = from_conds((Node *) ((FromExpr *) n)->fromlist, acc);
		acc = list_concat(acc, list_copy((List *) ((FromExpr *) n)->quals));
	}
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;

		acc = from_conds(j->larg, acc);
		if (j->jointype == JOIN_INNER)
		{
			acc = from_conds(j->rarg, acc);
			if (j->quals != NULL)
				acc = list_concat(acc, IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals));
		}
	}
	return acc;
}

static bool
from_same(PlannerInfo *pa, Node *a, Node *b, Relids *outer, bool nullable)
{
	if (a == NULL || b == NULL)
		return a == b;
	if (IsA(a, List))
	{
		ListCell   *la,
				   *lb;

		if (!IsA(b, List) || list_length((List *) a) != list_length((List *) b))
			return false;
		forboth(la, (List *) a, lb, (List *) b)
			if (!from_same(pa, lfirst(la), lfirst(lb), outer, nullable))
				return false;
		return true;
	}
	if (IsA(a, FromExpr))
		return IsA(b, FromExpr) && (!nullable || equal(((FromExpr *) a)->quals, ((FromExpr *) b)->quals)) &&
			from_same(pa, (Node *) ((FromExpr *) a)->fromlist, (Node *) ((FromExpr *) b)->fromlist, outer, nullable);
	if (IsA(a, JoinExpr))
	{
		JoinExpr   *ja = (JoinExpr *) a,
				   *jb;

		if (!IsA(b, JoinExpr))
			return false;
		jb = (JoinExpr *) b;
		if (ja->jointype != jb->jointype)
		{
			RangeTblEntry *child;

			if (!((ja->jointype == JOIN_LEFT && jb->jointype == JOIN_INNER) || (ja->jointype == JOIN_INNER && jb->jointype == JOIN_LEFT)) ||
				!IsA(ja->rarg, RangeTblRef))
				return false;
			child = rt_fetch(((RangeTblRef *) ja->rarg)->rtindex, pa->parse->rtable);
			if (child->rtekind != RTE_SUBQUERY || !child->lateral || child->subquery->limitCount == NULL)
				return false;
			if (jb->jointype == JOIN_LEFT)
				*outer = bms_add_member(*outer, ((RangeTblRef *) ja->rarg)->rtindex);
		}
		/* the conditions of inner joins are compared as one WHERE (same_scan); an outer join's ON is its own, and one that the other has inner has none */
		return ja->isNatural == jb->isNatural && ja->rtindex == jb->rtindex && equal(ja->usingClause, jb->usingClause) &&
			(ja->jointype == JOIN_INNER && jb->jointype == JOIN_INNER && !nullable ? true :
			 ja->jointype == JOIN_INNER && !nullable ? jb->quals == NULL :
			 jb->jointype == JOIN_INNER && !nullable ? ja->quals == NULL : equal(ja->quals, jb->quals)) &&
			equal(ja->alias, jb->alias) &&
			from_same(pa, ja->larg, jb->larg, outer, nullable) &&
			from_same(pa, ja->rarg, jb->rarg, outer, nullable || ja->jointype != JOIN_INNER || jb->jointype != JOIN_INNER);
	}
	return equal(a, b);
}

bool
same_scan(PlannerInfo *a, PlannerInfo *b, Relids *outer)
{
	ListCell   *la,
			   *lb;
	Relids		live;
	Index		rti = 0;

	List	   *ca,
			   *cb;

	if (list_length(a->parse->rtable) != list_length(b->parse->rtable) ||
		!from_same(a, (Node *) a->parse->jointree->fromlist, (Node *) b->parse->jointree->fromlist, outer, false))
		return false;
	ca = from_conds((Node *) a->parse->jointree->fromlist, NIL);
	cb = from_conds((Node *) b->parse->jointree->fromlist, NIL);
	foreach_ptr(Node, q, ca)
		if (!list_member(cb, q))
			return false;
	foreach_ptr(Node, q, cb)
		if (!list_member(ca, q))
			return false;
	live = get_relids_in_jointree((Node *) a->parse->jointree, false, false);
	forboth(la, a->parse->rtable, lb, b->parse->rtable)
	{
		RangeTblEntry same = *(RangeTblEntry *) lfirst(la),
				   *rb = lfirst(lb);

		same.eref = rb->eref;
		rti++;
		/* a sub-select that was not pulled up is joined: its rows are the arm's lookup, so its query must be the same */
		if (same.rtekind != rb->rtekind || (!arm_private_rte(same.rtekind) && !equal(&same, rb)) ||
			(same.rtekind == RTE_SUBQUERY && bms_is_member(rti, live) && !equal(same.subquery, rb->subquery)))
			return false;
	}
	return true;
}

/* grouping sets null a column in the sets that lack it: PostgreSQL marks the columns of a pulled up sub-select with a placeholder, in the joins too */
Node *
strip_ph(Node *n)
{
	while (IsA(n, PlaceHolderVar))
		n = (Node *) ((PlaceHolderVar *) n)->phexpr;
	return n;
}

bool
null_const(Node *e)
{
	return IsA(e, Const) && ((Const *) e)->constisnull;
}

/* `operand op constant` (either side) on an int8 or float8 operand: the operand, the comparison and the constant in *out */
bool
cond_split(Node *q, Node **operand, SiftAggCond *out)
{
	Oid			lt,
				rt,
				vtype;
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *k;
	int			cmp;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	if ((cmp = builtin_cmp(op->opno, &lt, &rt)) < 0)
		return false;
	l = strip_relabel(strip_ph(linitial(op->args)));
	r = strip_relabel(strip_ph(lsecond(op->args)));
	if (IsA(r, Const))
	{
		*operand = l;
		k = (Const *) r;
	}
	else if (IsA(l, Const))
	{
		*operand = r;
		k = (Const *) l;
		cmp = commute(cmp);
	}
	else
		return false;
	vtype = exprType(*operand);
	out->op = cmp;
	if (k->constisnull)
		return false;
	if (vtype == FLOAT8OID && k->consttype == FLOAT8OID)
	{
		out->isint = false;
		out->cd = DatumGetFloat8(k->constvalue);
	}
	else if (vtype == INT8OID && (k->consttype == INT8OID || k->consttype == INT4OID || k->consttype == INT2OID))
	{
		out->isint = true;
		out->ci = k->consttype == INT8OID ? DatumGetInt64(k->constvalue) : k->consttype == INT4OID ?
			DatumGetInt32(k->constvalue) : DatumGetInt16(k->constvalue);
	}
	else
		return false;
	return true;
}

/* `column op constant` on an output column of the arms: the column in out->col */
bool
outer_cond(Node *q, Index srti, SiftAggCond *out)
{
	Node	   *x;

	if (!cond_split(q, &x, out) || !IsA(x, Var) || ((Var *) x)->varno != srti || ((Var *) x)->varlevelsup != 0)
		return false;
	out->col = ((Var *) x)->varattno - 1;
	return true;
}
