/*
 * agg/join/pairs.c - the expressions of a statement over the pairs of a join (agg/join/pairs.h).
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"

#include "agg/join/pairs.h"

typedef struct PairCtx
{
	V6Plan	   *p;
	bool		ok;
} PairCtx;

/* the place of the column a Var is among the columns of the pairs, which it is added to when new; -1 when the pairs cannot carry it */
static int
pair_col(V6Plan *p, const Var *v)
{
	SiftV6Ext  *x = p->xseed;
	int			i;
	bool		ok;

	for (i = 0; i < x->nrcol; i++)
		if (p->pair_varno[i] == v->varno && p->pair_attno[i] == v->varattno)
			return i;
	if (v->varlevelsup != 0 || i == SIFT_V6_RESID_COLS)
		return -1;
	if (p->elem_rel != NULL && v->varno == p->elem_rel->relid)
		ok = sift_v6_unnest_column(p, (const Expr *) v, &x->rcol[i]);
	else
		ok = sift_v6_plan_read_column(p, (const Expr *) v, true, &x->rcol[i]);
	if (!ok)
		return -1;
	p->pair_varno[i] = v->varno;
	p->pair_attno[i] = v->varattno;
	x->nrcol++;
	return i;
}

static Node *
pair_var(Node *n, PairCtx *c)
{
	Var		   *nv;
	int			i;

	if (n == NULL || !c->ok)
		return n;
	if (IsA(n, PlaceHolderVar) || IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc))
	{
		c->ok = false;
		return n;
	}
	if (!IsA(n, Var))
		return expression_tree_mutator(n, pair_var, c);
	if ((i = pair_col(c->p, (const Var *) n)) < 0)
	{
		c->ok = false;
		return n;
	}
	nv = copyObject((Var *) n);
	nv->varno = nv->varnosyn = 1;
	nv->varattno = nv->varattnosyn = i + 1;
	nv->varnullingrels = NULL;
	return (Node *) nv;
}

Node *
sift_v6_pair_expr(V6Plan *p, Node *e)
{
	PairCtx		c = {p, true};
	Node	   *out = pair_var(e, &c);

	return c.ok ? out : NULL;
}
