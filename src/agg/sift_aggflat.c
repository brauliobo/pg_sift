/*
 * sift_aggflat.c - the lateral sub-selects PostgreSQL leaves in place, pulled up where the query level allows it.
 *
 * `a INNER JOIN LATERAL (SELECT ... FROM jsonb_each(a.doc) r LEFT JOIN m ON m.k = a.k AND m.x = r.key) s ON true` is the join of a with
 * the function and the table m: `(a JOIN jsonb_each(a.doc) r) LEFT JOIN m ON ...`. PostgreSQL does not pull that sub-select up when its
 * query ends up on the nullable side of an outer join of the enclosing query (`dates LEFT JOIN (that query)`: its columns would need the
 * outer join's nulling), and the scan sees a correlated child that needs the whole document. Inside its own query level the pull-up is
 * valid, so the planner hook does it there, plans the query both ways and keeps the flat one only if it runs in the scan and costs less.
 * A sub-select with an outer join is joined to the left side through its leftmost table, so that no outer join condition reads a table
 * outside the join: the shape PostgreSQL plans when it is written by hand.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"

#include "sift_agg.h"

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

static bool flatten_query(Query *q, bool apply);

/*
 * The joins that are not on the nullable side of an outer join of this level (a pulled-up column needs no nulling there). apply false only
 * tells whether there is a lateral sub-select to pull up. The node that takes the place of n.
 */
static Node *
flatten_jointree(Query *q, Node *n, bool apply, List **pulled, bool *changed)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, FromExpr))
	{
		FromExpr   *f = (FromExpr *) n;
		List	   *items = NIL;

		foreach_ptr(Node, x, f->fromlist)
			items = lappend(items, flatten_jointree(q, x, apply, pulled, changed));
		f->fromlist = items;
	}
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;
		RangeTblEntry *rte;

		if (j->jointype == JOIN_INNER || j->jointype == JOIN_LEFT)
			j->larg = flatten_jointree(q, j->larg, apply, pulled, changed);
		if (j->jointype == JOIN_INNER)
			j->rarg = flatten_jointree(q, j->rarg, apply, pulled, changed);
		if ((rte = lateral_inner_child(q, j)) != NULL)
		{
			*changed = true;
			/* the pulled-up joins may hold another one */
			return apply ? flatten_jointree(q, pull_up_lateral(q, j, rte, pulled), apply, pulled, changed) : n;
		}
	}
	return n;
}

static bool
flatten_query(Query *q, bool apply)
{
	bool		changed = false;
	List	   *pulled = NIL;

	if (q->commandType != CMD_SELECT)
		return false;
	foreach_node(CommonTableExpr, cte, q->cteList)
		changed |= flatten_query(castNode(Query, cte->ctequery), apply);
	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind == RTE_SUBQUERY)
			changed |= flatten_query(rte->subquery, apply);
	q->jointree = (FromExpr *) flatten_jointree(q, (Node *) q->jointree, apply, &pulled, &changed);
	/* the copy ReplaceVarsFromTargetList makes takes the place of the query; a sub-select may read the columns of an earlier one */
	foreach_ptr(Pulled, p, pulled)
	{
		*q = *(Query *) ReplaceVarsFromTargetList((Node *) q, p->rti, 0, p->rte, p->targetlist, 0, REPLACEVARS_REPORT_ERROR, 0, NULL);
		foreach_ptr(Pulled, later, pulled)
			later->targetlist = (List *) ReplaceVarsFromTargetList((Node *) later->targetlist, p->rti, 0, p->rte, p->targetlist, 0, REPLACEVARS_REPORT_ERROR, 0, NULL);
	}
	return changed;
}

bool
sift_agg_has_flattenable_laterals(Query *q)
{
	return flatten_query(q, false);
}

void
sift_agg_flatten_laterals(Query *q)
{
	flatten_query(q, true);
}
