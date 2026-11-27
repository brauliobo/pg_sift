/*
 * agg/join/pairs.c - the expressions of a statement over the pairs of a join (agg/join/pairs.h).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/join/pairs.h"

typedef struct PairCtx
{
	V6Plan	   *p;
	bool		ok;
} PairCtx;

/* a column the pairs carry: a column of the fact table, of the element or of a dimension, or what the index holds of an expression of the fact table (jsonb_exists(doc, 'k') of an expression column) */
static bool
pair_source(V6Plan *p, Node *n, SiftV6Col *out)
{
	const Var  *v = (const Var *) n;

	if (!IsA(n, Var))
		return bms_equal(pull_varnos(p->root, n), p->rel->relids) && sift_v6_read_of(p, (const Expr *) n, false, out) && out->src == SV6SRC_ATTR;
	if (v->varlevelsup != 0)
		return false;
	if (p->elem_rel != NULL && v->varno == p->elem_rel->relid)
		return sift_v6_unnest_column(p, (const Expr *) v, out);
	return sift_v6_plan_read_column(p, (const Expr *) v, true, out);
}

/* the place of the column n is among the columns of the pairs, which it is added to when new; -1 when the pairs cannot carry it */
static int
pair_col(V6Plan *p, Node *n)
{
	SiftV6Ext  *x = p->xseed;
	int			i;

	for (i = 0; i < x->nrcol; i++)
		if (equal(p->pair_src[i], n))
			return i;
	if (i == SIFT_V6_RESID_COLS || !pair_source(p, n, &x->rcol[i]))
		return -1;
	p->pair_src[i] = n;
	x->nrcol++;
	return i;
}

/* n as the column of the pairs it is (a Var without the outer joins that null it, which the pairs carry as NULLs, or a call the index holds), or NULL when it is none */
static Var *
pair_column(PairCtx *c, Node *n)
{
	Node	   *src = copyObject(n);
	Var		   *nv;
	int			i;

	if (IsA(src, Var))
		((Var *) src)->varnullingrels = NULL;
	else if (!(IsA(src, FuncExpr) || IsA(src, OpExpr)) || !contain_var_clause(src))
		return NULL;
	if ((i = pair_col(c->p, src)) < 0)
		return NULL;
	nv = makeVar(1, (AttrNumber) (i + 1), exprType(n), exprTypmod(n), exprCollation(n), 0);
	nv->varnosyn = 1;
	nv->varattnosyn = nv->varattno;
	return nv;
}

static Node *
pair_var(Node *n, PairCtx *c)
{
	Var		   *nv;

	if (n == NULL || !c->ok)
		return n;
	if (IsA(n, PlaceHolderVar) || IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc))
	{
		c->ok = false;
		return n;
	}
	if ((nv = pair_column(c, n)) != NULL)
		return (Node *) nv;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, pair_var, c);
	c->ok = false;
	return n;
}

Node *
sift_v6_pair_expr(V6Plan *p, Node *e)
{
	PairCtx		c = {p, true};
	Node	   *out = pair_var(e, &c);

	return c.ok ? out : NULL;
}
