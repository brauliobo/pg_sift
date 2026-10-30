/*
 * agg/from/reads.c - which leaves a predicate or a list of tests reads
 * And what a statement reads of a leaf, unconditionally or in an order the scan may refuse.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"

#include "agg/part.h"
#include "agg/from/int.h"


/* is the node the read of the scalar leaf: the cast of `doc -> 'k'` or `doc ->> 'k'` to the leaf's type, the path of the leaf? */
static bool
leaf_read(Node *n, const Unconditional *u)
{
	const SiftSrcLeaf *l = leaf_of(u->c, u->leaf);
	JsonRef		r = {0};
	Node	   *arg;

	if (u->as_text && IsA(n, CoerceViaIO))
		arg = (Node *) ((CoerceViaIO *) n)->arg;
	else if (!u->as_text && IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) != InvalidOid)
		arg = linitial(((FuncExpr *) n)->args);
	else
		return false;
	if (exprType(n) != l->typid || !json_access(u->c, arg, &r) || r.as_text != u->as_text)
		return false;
	/* the member of an element of the unnested array: `p ->> 'k'` of jsonb_array_elements */
	if (l->parent >= 0)
		return r.att == 0 && r.fcol == 1 && u->c->func_kind == FK_ELEMENTS && list_length(r.path) == 1 && strcmp(strVal(linitial(r.path)), l->name) == 0;
	return r.att == l->attno && paths_equal(r.path, l->path);
}

/* does the expression evaluate what u looks for, for every row that reaches it? A CASE branch, a FILTER, the later operands of AND, OR and COALESCE do for some only */
bool
unconditional(Node *n, Unconditional *u)
{
	AggCtx	   *c = u->c;

	if (n == NULL)
		return false;
	if (u->leaf >= 0 && leaf_read(n, u))
		return true;
	if (u->leaf < 0 && IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) == c->cast_want)
	{
		Node	   *arg = strip_relabel(linitial(((FuncExpr *) n)->args));

		return IsA(arg, Var) && ((Var *) arg)->varno == c->func_rti && ((Var *) arg)->varattno == 2 && ((Var *) arg)->varlevelsup == 0;
	}
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;

		/* the FILTER is tested for every row that reaches the aggregate; the arguments for the rows it keeps */
		if (unconditional((Node *) a->aggfilter, u))
			return true;
		if (a->aggfilter != NULL)
			return false;
		foreach_ptr(Node, x, list_concat_copy(a->aggdirectargs, a->args))
			if (unconditional(IsA(x, TargetEntry) ? (Node *) ((TargetEntry *) x)->expr : x, u))
				return true;
		return false;
	}
	if (IsA(n, CaseExpr))
		return unconditional((Node *) ((CaseExpr *) n)->arg, u);
	if (IsA(n, CoalesceExpr))
		return unconditional(linitial(((CoalesceExpr *) n)->args), u);
	if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop != NOT_EXPR)
		return unconditional(linitial(((BoolExpr *) n)->args), u);
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		return false;
	return expression_tree_walker(n, unconditional, u);
}

/* counts the reads of the leaf in the tree, a sub-select's and a function's arguments too */
bool
count_leaf_reads(Node *n, Unconditional *u)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
		/* PostgreSQL 18 keeps the grouping expressions in an entry of the range table too: the same ones the targets have */
		return query_tree_walker((Query *) n, count_leaf_reads, u,
#if PG_VERSION_NUM >= 180000
								 QTW_IGNORE_GROUPEXPRS
#else
								 0
#endif
			);
	if (leaf_read(n, u))
	{
		u->nread++;
		return false;
	}
	return expression_tree_walker(n, count_leaf_reads, u);
}

/* does a conjunct of the rows read the leaf, directly or in an expression of a combo? */
bool
pred_reads_leaf(AggCtx *c, const SiftAggPred *p, int leaf)
{
	if (p->kind == SAP_COMBO)
	{
		const ComboCtx *cb = list_nth(c->combos, p->leaf);

		for (int k = 0; k < cb->nl; k++)
			if (cb->leaf[k] == leaf)
				return true;
		return false;
	}
	if ((p->leaf >= 0 && p->leaf == leaf) || (p->leaf2 >= 0 && p->leaf2 == leaf))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_leaf(c, p->child[i], leaf))
			return true;
	return false;
}

/* does a conjunct of the rows read one of the leaves? Those rows would be dropped before the error of the list they hold is raised */
bool
pred_reads_any(const SiftAggPred *p, const Bitmapset *leaves)
{
	if (p->leaf >= 0 && bms_is_member(p->leaf, leaves))
		return true;
	if (p->leaf2 >= 0 && bms_is_member(p->leaf2, leaves))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_any(p->child[i], leaves))
			return true;
	return false;
}

/* does a conjunct read one of the leaves, also through the leaves of a combo (an expression over them), which its `leaf` is the number of? */
bool
pred_reads_set(AggCtx *c, const SiftAggPred *p, const Bitmapset *leaves)
{
	if (p->kind == SAP_COMBO || p->kind == SAP_ELEM_EXISTS || p->kind == SAP_MATCHED)
	{
		const ComboCtx *cb = list_nth(c->combos, p->leaf);

		for (int k = 0; k < cb->nl; k++)
			if (cb->leaf[k] >= 0 && bms_is_member(cb->leaf[k], leaves))
				return true;
	}
	else if ((p->leaf >= 0 && bms_is_member(p->leaf, leaves)) || (p->leaf2 >= 0 && bms_is_member(p->leaf2, leaves)))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_set(c, p->child[i], leaves))
			return true;
	return false;
}

/* is every read of list grp a test of the joined tuples (SAP_ELEM_EXISTS), which raises the error of a row it reaches itself? */
bool
list_read_by_tuple_tests(const SiftAggSpec *s, int grp)
{
	bool		found = false;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		if (p->kind == SAP_ELEM_EXISTS && p->leaf2 == grp && p->pass_poison)
			found = true;
		else if (p->kind == SAP_ANY_ELEM && p->leaf == grp)
			return false;
	}
	return found;
}

/* the leaf is left to the normal plan because of the order of the conditions, which reads_overflow names */
SiftSrcRecheck
order_refused(AggCtx *c, int m)
{
	c->order_refused = bms_add_member(c->order_refused, m);
	return SRK_NONE;
}

/*
 * Does PostgreSQL test every condition of the WHERE that does not read the leaf (u) before each one that does? A scan sorts its conditions by their
 * cost, the written order among equals (order_qual_clauses), and stops at the first that drops the row, so a row another condition drops never
 * reaches the cast of the leaf, however the plan reaches the table. The scan raises for the rows that every other condition keeps, so the two raise
 * for the same rows exactly when the others come first.
 */
bool
others_first(AggCtx *c, const Unconditional *u)
{
	Node	   *quals = (Node *) c->root->parse->jointree->quals;
	List	   *conds = quals == NULL ? NIL : IsA(quals, List) ? (List *) quals : list_make1(quals);
	int			n = list_length(conds);
	double	   *cost = palloc(Max(n, 1) * sizeof(double));
	bool	   *reads = palloc(Max(n, 1) * sizeof(bool));

	for (int i = 0; i < n; i++)
	{
		Unconditional t = *u;
		QualCost	qc;

		t.nread = 0;
		count_leaf_reads(list_nth(conds, i), &t);
		reads[i] = t.nread > 0;
		cost_qual_eval_node(&qc, list_nth(conds, i), c->root);
		cost[i] = qc.per_tuple;
	}
	for (int o = 0; o < n; o++)
		for (int r = 0; r < n; r++)
			if (!reads[o] && reads[r] && !(cost[o] < cost[r] || (cost[o] == cost[r] && o < r)))
				return false;
	return true;
}

/* counts the reads of the leaf in the arguments of the aggregates with a FILTER that does not read it: the aggregate evaluates them for the rows the FILTER keeps */
bool
filtered_reads(Node *n, Unconditional *u)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref) && ((Aggref *) n)->aggfilter != NULL)
	{
		Aggref	   *a = (Aggref *) n;
		Unconditional t = *u;

		t.nread = 0;
		count_leaf_reads((Node *) a->aggfilter, &t);
		if (t.nread == 0)
		{
			count_leaf_reads((Node *) a->args, u);
			count_leaf_reads((Node *) a->aggdirectargs, u);
		}
		return false;
	}
	return expression_tree_walker(n, filtered_reads, u);
}

/*
 * The aggregates that read leaf m (bit j: agg[j]), when each has a FILTER that cannot raise and reads no leaf that holds a bad value: its row raises
 * where one of them keeps it. 0: there is one without, or too many aggregates for the bits.
 */
uint64
gate_mask(AggCtx *c, const SiftAggSpec *s, int m)
{
	uint64		mask = 0;
	Bitmapset  *dirty = NULL;

	if (s->naggs > 64)
		return 0;
	for (int i = 0; i < c->lay->nleaves - c->nvleaf; i++)
		if (c->stats.ovf[i])
			dirty = bms_add_member(dirty, i);
	for (int j = 0; j < s->naggs; j++)
		if (bms_is_member(m, s->agg[j].reads))
		{
			const SiftAggPred *f = s->agg[j].filter;

			if (f == NULL || f->late || f->level != 0 || pred_reads_set(c, f, dirty))
				return 0;
			mask |= UINT64CONST(1) << j;
		}
	return mask;
}
