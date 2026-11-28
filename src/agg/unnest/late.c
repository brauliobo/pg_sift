/*
 * agg/unnest/late.c - the keys and aggregates that read a late column of the element, evaluated for a pair whose member raised (agg/unnest/late.h).
 */
/* check-memory: allow-allocs the items follow the expressions of one statement, not the rows of the table */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/ext.h"
#include "agg/unnest/late.h"
#include "agg/unnest/jsonb.h"

typedef struct ToExec
{
	Index		fact;
	Index		elem;
	bool		foreign;
	bool		elem_read;
} ToExec;

/* the Vars of the element as INNER_VAR, those of the table kept for the scan tuple; any other input of the expression is one the pair does not hold */
static Node *
to_exec(Node *n, ToExec *t)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = copyObject((Var *) n);

		t->foreign |= v->varlevelsup != 0 || (v->varno != t->fact && v->varno != t->elem);
		t->elem_read |= v->varno == t->elem;
		if (v->varno == t->elem)
			v->varno = INNER_VAR;
		v->varnullingrels = NULL;
		return (Node *) v;
	}
	t->foreign |= IsA(n, Param) || IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, PlaceHolderVar) || IsA(n, Aggref);
	return expression_tree_mutator(n, to_exec, t);
}

/* the item that reads the value for every pair */
static List *
always(void)
{
	return list_make2(makeBoolConst(true, false), NIL);
}

static List *
item_of(const V6Plan *p, Expr *filter, List *args)
{
	ToExec		t = {.fact = p->rel->relid,.elem = p->elem_rel->relid};
	Node	   *f = to_exec((Node *) filter, &t);
	Node	   *a = to_exec((Node *) args, &t);

	if (!t.elem_read)
		return NIL;
	if (t.foreign)
		return always();
	return list_make2(f, a);
}

static bool
has_late_column(const SiftV6XElem *e)
{
	for (int c = 0; c < e->ncols; c++)
		if (sift_v6_elem_late(e, c))
			return true;
	return false;
}

List *
sift_v6_late_probe_plan(const V6Plan *p)
{
	List	   *items = NIL;
	List	   *item;

	if (!has_late_column(&p->xseed->elem))
		return NIL;
	if (p->elem_rel == NULL || p->xseed->ndims > 0)
		return list_make1(always());
	foreach_ptr(Expr, key, sift_v6_group_exprs_of(p->root))
		if ((item = item_of(p, (Expr *) makeBoolConst(true, false), list_make1(key))) != NIL)
			items = lappend(items, item);
	foreach_ptr(Node, n, pull_var_clause((Node *) list_make2(p->out->reltarget->exprs, p->root->parse->havingQual), PVC_INCLUDE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS))
	{
		const Aggref *a = (const Aggref *) n;
		List	   *args = NIL;

		if (!IsA(n, Aggref))
			continue;
		foreach_node(TargetEntry, te, a->args)
			args = lappend(args, te->expr);
		if ((item = item_of(p, a->aggfilter != NULL ? a->aggfilter : (Expr *) makeBoolConst(true, false), args)) != NIL)
			items = lappend(items, item);
	}
	return items;
}

List *
sift_v6_late_where_plan(const V6Plan *p)
{
	ToExec		t = {.fact = p->rel->relid,.elem = p->elem_rel->relid};
	List	   *where = NIL;

	foreach_node(RestrictInfo, ri, sift_v6_quals_in_order(p->root, p->elem_rel->baserestrictinfo))
		where = lappend(where, to_exec((Node *) ri->clause, &t));
	SIFT_INVARIANT(!t.foreign, "the quals of the element the scan decides read the element alone");
	return where;
}

typedef struct LateItem
{
	ExprState  *filter;
	int			nargs;
	ExprState **args;
} LateItem;

struct SiftV6LateProbe
{
	int			nitems;
	LateItem   *item;
	const SiftV6RecFn *rec;
	TupleTableSlot *member;		/* the member of the pair as the function gives it: its key and value of jsonb_each, the element of jsonb_array_elements */
	ExprState  *where;			/* SiftV6Ext.late_where; NULL: none */
};

SiftV6LateProbe *
sift_v6_late_probe_open(const List *items, const List *where, EState *estate, const SiftV6RecFn *rec)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	SiftV6LateProbe *lp = palloc0(sizeof(SiftV6LateProbe));
	int			i = 0;

	lp->rec = rec;
	lp->member = ExecInitExtraTupleSlot(estate, rec->desc, &TTSOpsVirtual);
	lp->where = where != NIL ? ExecPrepareQual(unconstify(List *, where), estate) : NULL;
	lp->nitems = list_length(items);
	lp->item = palloc0(Max(lp->nitems, 1) * sizeof(LateItem));
	foreach_node(List, it, items)
	{
		LateItem   *li = &lp->item[i++];
		int			a = 0;

		li->filter = ExecPrepareQual(list_make1(linitial(it)), estate);
		li->nargs = list_length(lsecond(it));
		li->args = palloc0(Max(li->nargs, 1) * sizeof(ExprState *));
		foreach_ptr(Expr, arg, (List *) lsecond(it))
			li->args[a++] = ExecPrepareExpr(arg, estate);
	}
	MemoryContextSwitchTo(old);
	return lp;
}

static TupleTableSlot *
member_slot(SiftV6LateProbe *lp, uint32 k)
{
	const SiftV6RecFn *f = lp->rec;
	const int	v = f->fn >= SV6E_EACH ? 1 : 0;

	ExecClearTuple(lp->member);
	if (v == 1)
	{
		lp->member->tts_values[0] = f->key[k];
		lp->member->tts_isnull[0] = false;
	}
	lp->member->tts_values[v] = f->val[k];
	lp->member->tts_isnull[v] = f->val_null[k];
	return ExecStoreVirtualTuple(lp->member);
}

void
sift_v6_late_probe_check(SiftV6LateProbe *lp, ExprContext *econtext, uint32 k, ErrorData *err)
{
	econtext->ecxt_innertuple = member_slot(lp, k);
	for (int i = 0; i < lp->nitems; i++)
	{
		const LateItem *li = &lp->item[i];
		bool		isnull;

		if (!ExecQual(li->filter, econtext))
			continue;
		if (li->nargs == 0)
			ReThrowError(err);
		for (int a = 0; a < li->nargs; a++)
			(void) ExecEvalExprSwitchContext(li->args[a], econtext, &isnull);
	}
}

uint64 *
sift_v6_late_keep(SiftV6LateProbe *lp, SiftV6ElemEval *ee, SiftV6RecFn *rec, ExprContext *econtext, MemoryContext cx, SiftVec *ev, uint32 nel, ErrorData ***late)
{
	const bool	where = lp != NULL && lp->where != NULL;
	uint64	   *keep = NULL;

	*late = rec != NULL ? palloc0_array(ErrorData *, Max(nel, 1)) : NULL;
	if (where)
		sift_v6_recfn_values(rec, NULL, cx, ev, nel, *late);
	if (ee != NULL && nel > 0)
	{
		keep = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
		sift_v6_elem_eval(ee, ev, 0, nel, NULL, keep);
	}
	if (!where)
	{
		if (rec != NULL)
			sift_v6_recfn_values(rec, keep, cx, ev, nel, *late);
		return keep;
	}
	for (uint32 k = 0; k < nel; k++)
		if ((*late)[k] != NULL)
		{
			econtext->ecxt_innertuple = member_slot(lp, k);
			if (ExecQual(lp->where, econtext))
				keep[k >> 6] |= UINT64CONST(1) << (k & 63);
			else
			{
				keep[k >> 6] &= ~(UINT64CONST(1) << (k & 63));
				(*late)[k] = NULL;
			}
		}
	return keep;
}
