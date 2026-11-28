/*
 * agg/join/pairs.c - the expressions of a statement over the pairs of a join (agg/join/pairs.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/placeholder.h"
#include "utils/fmgroids.h"

#include "agg/exists_plan.h"
#include "agg/expr/pg.h"
#include "agg/join/pairs.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"

typedef struct PairCtx
{
	V6Plan	   *p;
	int			in_sub;			/* below a sub-plan the evaluator may run (its parameters are its own or of an InitPlan) */
	bool		ok;
} PairCtx;

/* a column the pairs carry: a column of the fact table, of the element (or the cast of the value of a member) or of a dimension, what the index holds of an expression of the fact table (jsonb_exists(doc, 'k') of an expression column),
 * or the flag of a test of the row (INDEX_VAR) */
static bool
pair_source(V6Plan *p, Node *n, SiftV6Col *out)
{
	const Var  *v = (const Var *) n;

	if (IsA(n, Var) && v->varno == INDEX_VAR)
	{
		*out = (SiftV6Col) {.attno = v->varattno,.typid = BOOLOID};
		return sift_v6_col_is_flag(out) && v->varattno - SIFT_V6_XFLAG_VATT < p->xseed->ntests;
	}
	if (p->elem_rel != NULL && bms_equal(pull_varnos(p->root, n), p->elem_rel->relids))
		return sift_v6_unnest_column(p, (const Expr *) n, out) || sift_v6_unnest_value_int(p, (const Expr *) n, out);
	if (!IsA(n, Var))
		return bms_equal(pull_varnos(p->root, n), p->rel->relids) && sift_v6_read_of(p, (const Expr *) n, false, out) && out->src == SV6SRC_ATTR;
	if (v->varlevelsup != 0)
		return false;
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

/* a copy of n whose Vars name no outer join that nulls them */
static Node *
unnulled(Node *n, void *cx)
{
	if (n != NULL && IsA(n, Var))
	{
		Var		   *v = copyObject((Var *) n);

		v->varnullingrels = NULL;
		return (Node *) v;
	}
	return expression_tree_mutator(n, unnulled, cx);
}

/* the integer cast of the float8 the pairs hold of the value of a member (sift_v6_unnest_value_int), through numeric as the cast of the jsonb's number */
static Node *
as_type(Var *v, Oid type)
{
	const Oid	fn = type == INT2OID ? F_INT2_NUMERIC : type == INT4OID ? F_INT4_NUMERIC : F_INT8_NUMERIC;
	FuncExpr   *num = makeFuncExpr(F_NUMERIC_FLOAT8, NUMERICOID, list_make1(v), InvalidOid, InvalidOid, COERCE_EXPLICIT_CAST);

	return v->vartype == type ? (Node *) v : (Node *) makeFuncExpr(fn, type, list_make1(num), InvalidOid, InvalidOid, COERCE_EXPLICIT_CAST);
}

/* n as the column of the pairs it is (a Var without the outer joins that null it, which the pairs carry as NULLs, or a call the index holds), or NULL when it is none */
static Node *
pair_column(PairCtx *c, Node *n)
{
	Node	   *src = unnulled(n, NULL);
	Var		   *nv;
	int			i;

	if (!IsA(src, Var) && (!(IsA(src, FuncExpr) || IsA(src, OpExpr)) || !contain_var_clause(src)))
		return NULL;
	if ((i = pair_col(c->p, src)) < 0)
		return NULL;
	nv = makeVar(1, (AttrNumber) (i + 1), c->p->xseed->rcol[i].typid, exprTypmod(n), exprCollation(n), 0);
	nv->varnosyn = 1;
	nv->varattnosyn = nv->varattno;
	return as_type(nv, exprType(n));
}

static Node *
pair_var(Node *n, PairCtx *c)
{
	Node	   *nv;

	if (n == NULL || !c->ok)
		return n;
	if (IsA(n, SubPlan) && sift_v6_pgexpr_plan_ok(c->p->root, (Expr *) n))
	{
		Node	   *out;

		c->in_sub++;
		out = expression_tree_mutator(n, pair_var, c);
		c->in_sub--;
		return out;
	}
	if (IsA(n, Param) && c->in_sub > 0)
		return n;
	if (IsA(n, PlaceHolderVar) && sift_v6_placeholder_is_expr(c->p->root, (PlaceHolderVar *) n))
		return pair_var((Node *) ((PlaceHolderVar *) n)->phexpr, c);
	if (IsA(n, PlaceHolderVar) || IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc))
	{
		c->ok = false;
		return n;
	}
	if ((nv = pair_column(c, n)) != NULL)
		return nv;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, pair_var, c);
	c->ok = false;
	return n;
}

bool
sift_v6_placeholder_is_expr(PlannerInfo *root, PlaceHolderVar *phv)
{
	const Node *e = (const Node *) phv->phexpr;
	const Relids vars = pull_varnos(root, (Node *) e);

	return phv->phlevelsup == 0 && !bms_is_empty(vars) && bms_is_subset(vars, find_placeholder_info(root, phv)->ph_eval_at) && !contain_nonstrict_functions((Node *) e) &&
		!contain_volatile_functions((Node *) e) && !contain_subplans((Node *) e);
}

Node *
sift_v6_pair_expr(V6Plan *p, Node *e)
{
	PairCtx		c = {.p = p,.ok = true};
	Node	   *out = pair_var(e, &c);

	return c.ok ? out : NULL;
}

static bool
opaque_placeholder(Node *n, void *context)
{
	PlannerInfo *root = (PlannerInfo *) context;

	return n != NULL && ((IsA(n, PlaceHolderVar) && !sift_v6_placeholder_is_expr(root, (PlaceHolderVar *) n)) || expression_tree_walker(n, opaque_placeholder, context));
}

/*
 * a placeholder other than a strict expression (sift_v6_placeholder_is_expr) in a condition of the join or of a relation, or in a class of equal expressions a condition made (not the
 * order of a key): the keys and conditions of the pairs read columns, the other placeholders are the output's (sift_v6_placeholder_values)
 */
bool
sift_v6_join_placeholder_in_quals(PlannerInfo *root, Relids base)
{
	int			r = -1;

	if (root->placeholder_list == NIL)
		return false;
	while ((r = bms_next_member(base, r)) >= 0)
	{
		const RelOptInfo *rel = root->simple_rel_array[r];

		if (rel == NULL)
			continue;
		foreach_node(RestrictInfo, ri, list_concat_copy(rel->baserestrictinfo, rel->joininfo))
			if (opaque_placeholder((Node *) ri->clause, root))
				return true;
	}
	foreach_node(EquivalenceClass, ec, root->eq_classes)
		foreach_node(EquivalenceMember, em, ec->ec_members)
			if (ec->ec_sources != NIL && opaque_placeholder((Node *) em->em_expr, root))
				return true;
	return false;
}

static bool
seed_conditions(V6Plan *p, const List *quals, List **out)
{
	foreach_node(RestrictInfo, ri, quals)
	{
		Node	   *e;

		if (contain_volatile_functions((Node *) ri->clause) || (e = sift_v6_pair_expr(p, (Node *) ri->clause)) == NULL)
			return false;
		*out = lappend(*out, e);
	}
	return true;
}

/* the result of the CASE of a split as the flag of its test; NULL when the plan has no test of it */
static Node *
split_result(const V6Plan *p, const SiftV6ExistsSplit *sx, const Const *value)
{
	const int	flag = sift_v6_unnest_split_flag(p, sx->sp, value);

	return flag < 0 ? NULL : (Node *) makeVar(INDEX_VAR, SIFT_V6_XFLAG_VATT + flag, BOOLOID, -1, InvalidOid, 0);
}

/* the condition of the pairs a split is: its CASE over the flags of its tests */
static bool
seed_split(V6Plan *p, const SiftV6ExistsSplit *sx)
{
	CaseExpr   *cx = copyObject(unconstify(CaseExpr *, sx->cx));
	Node	   *e;

	cx->casetype = BOOLOID;
	cx->casecollid = InvalidOid;
	foreach_node(CaseWhen, w, cx->args)
		if ((w->result = (Expr *) split_result(p, sx, (const Const *) w->result)) == NULL)
			return false;
	if ((cx->defresult = (Expr *) split_result(p, sx, sift_v6_exists_split_else(sx))) == NULL || (e = sift_v6_pair_expr(p, (Node *) cx)) == NULL)
		return false;
	p->xseed->resid = lappend(p->xseed->resid, e);
	return true;
}

bool
sift_v6_join_seed_resid(V6Plan *p, const SiftV6JoinShape *sh)
{
	if (!seed_conditions(p, sh->resid, &p->xseed->resid))
		return false;
	foreach_ptr(SiftV6ExistsSplit, sx, sh->splits)
		if (!seed_split(p, sx))
			return false;
	for (int d = 0; d < sh->nd; d++)
		if (!seed_conditions(p, sh->dim[d].on, &p->xseed->on[d]))
			return false;
	return true;
}
