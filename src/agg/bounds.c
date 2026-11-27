/*
 * agg/bounds.c - the bounds the conjuncts of a WHERE clause put on the integer and date columns (v6_vexpr.h), for a source to leave the granules and parts they rule out unread.
 * A comparison of a column with a constant or a parameter, and `= ANY (constants)`, give a lowest and a highest value; the conjuncts a column has more of are kept as they are.
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/vexpr.h"

typedef struct BoundCtx
{
	Index		relid;
	ParamListInfo params;
	SiftV6Input *in;
	int		   *n;
	SiftV6Bound *bound;
} BoundCtx;

/* The value this execution gives $n; false when the path is being made (no parameters) or the parameter is not of the type the plan was made for */
bool
sift_v6_param_value(ParamListInfo params, const Param *p, Datum *value, bool *isnull)
{
	ParamExternData workspace;
	const ParamExternData *prm;

	if (params == NULL || p->paramkind != PARAM_EXTERN || p->paramid < 1 || p->paramid > params->numParams)
		return false;
	prm = params->paramFetch != NULL ? params->paramFetch(params, p->paramid, false, &workspace) : &params->params[p->paramid - 1];
	if (prm == NULL || prm->ptype != p->paramtype)
		return false;
	*value = prm->value;
	*isnull = prm->isnull;
	return true;
}


static Node *
bind_param(Node *n, void *context)
{
	const Param *p = (const Param *) n;
	Datum		value;
	bool		isnull;
	int16		typlen;
	bool		byval;

	if (n == NULL || !IsA(n, Param) || p->paramkind != PARAM_EXTERN)
		return expression_tree_mutator(n, bind_param, context);
	if (!sift_v6_param_value(context, p, &value, &isnull))
		return copyObject(n);
	get_typlenbyval(p->paramtype, &typlen, &byval);
	return (Node *) makeConst(p->paramtype, p->paramtypmod, p->paramcollid, typlen, isnull ? (Datum) 0 : datumCopy(value, byval, typlen), isnull, byval);
}

List *
sift_v6_bind_params(List *clauses, ParamListInfo params)
{
	return (List *) bind_param((Node *) clauses, params);
}

/* ---- the bounds the conjuncts put on the columns, for the source to leave granules unread ---- */

static bool
bound_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

/* the integer a constant of the bound's types is, as a column of the type holds it */
static int64
bound_int(Oid t, Datum d)
{
	return t == INT2OID ? DatumGetInt16(d) : t == INT4OID || t == DATEOID ? DatumGetInt32(d) : DatumGetInt64(d);
}

/* a constant, or the value this execution gives a parameter, that is no NULL, of a type that bounds a column of vartype */
static bool
bound_operand(const BoundCtx *c, const Node *e, Oid vartype, int64 *v)
{
	Datum		d;
	bool		isnull;
	Oid			t;

	if (IsA(e, Const))
	{
		d = ((const Const *) e)->constvalue;
		isnull = ((const Const *) e)->constisnull;
		t = ((const Const *) e)->consttype;
	}
	else if (IsA(e, Param) && sift_v6_param_value(c->params, (const Param *) e, &d, &isnull))
		t = ((const Param *) e)->paramtype;
	else
		return false;
	if (isnull || !bound_type(t) || (vartype == t ? false : !((vartype == INT2OID || vartype == INT4OID || vartype == INT8OID) && (t == INT2OID || t == INT4OID || t == INT8OID))))
		return false;
	*v = bound_int(t, d);
	return true;
}

static void
bound_add(BoundCtx *c, const Var *var, bool has_lo, int64 lo, bool has_hi, int64 hi)
{
	int			slot;

	if (*c->n == SIFT_V6_MAX_BOUNDS || var->varno != c->relid || var->varlevelsup != 0 || var->varattno <= 0 || !bound_type(var->vartype) ||
		(slot = sift_v6_input_add_var(c->in, var)) < 0 || c->in->col[slot].scale != 0)
		return;
	c->bound[(*c->n)++] = (SiftV6Bound) {.slot = slot,.has_lo = has_lo,.lo = lo,.has_hi = has_hi,.hi = hi};
}

static void
bound_of_op(BoundCtx *c, const OpExpr *op)
{
	const char *name = get_opname(op->opno);
	const Node *l = linitial(op->args),
			   *r = lsecond(op->args);
	bool		swapped = !IsA(l, Var);
	const Var  *var = (const Var *) (swapped ? r : l);
	int64		v;
	int			cmp;

	if (list_length(op->args) != 2 || name == NULL || !sift_v6_cmp_op_of(name, &cmp) || !IsA(var, Var) || !bound_type(var->vartype) ||
		get_func_namespace(get_opcode(op->opno)) != PG_CATALOG_NAMESPACE || !bound_operand(c, swapped ? l : r, var->vartype, &v))
		return;
	if (swapped)
		cmp = cmp == SCMP_LT ? SCMP_GT : cmp == SCMP_LE ? SCMP_GE : cmp == SCMP_GT ? SCMP_LT : cmp == SCMP_GE ? SCMP_LE : cmp;
	if (cmp == SCMP_EQ)
		bound_add(c, var, true, v, true, v);
	else if (cmp == SCMP_GE || (cmp == SCMP_GT && v < INT64_MAX))
		bound_add(c, var, true, cmp == SCMP_GT ? v + 1 : v, false, 0);
	else if (cmp == SCMP_LE || (cmp == SCMP_LT && v > INT64_MIN))
		bound_add(c, var, false, 0, true, cmp == SCMP_LT ? v - 1 : v);
}

static void
bound_of_any(BoundCtx *c, const ScalarArrayOpExpr *sa)
{
	const char *name = get_opname(sa->opno);
	const Var  *var = linitial(sa->args);
	const int64 *vals;
	int			n;

	if (!sa->useOr || name == NULL || strcmp(name, "=") != 0 || !IsA(var, Var) || !bound_type(var->vartype) ||
		!sift_v6_array_ints(CurrentMemoryContext, lsecond(sa->args), var->vartype, c->params, &vals, &n) || n == 0)
		return;
	bound_add(c, var, true, vals[0], true, vals[n - 1]);
}

static void
bounds_of(BoundCtx *c, const Node *clause)
{
	if (IsA(clause, RestrictInfo))
		clause = (const Node *) ((const RestrictInfo *) clause)->clause;
	if (IsA(clause, OpExpr))
		bound_of_op(c, (const OpExpr *) clause);
	else if (IsA(clause, ScalarArrayOpExpr))
		bound_of_any(c, (const ScalarArrayOpExpr *) clause);
}

void
sift_v6_bounds_of(const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, int *n, SiftV6Bound *bound)
{
	BoundCtx	c = {.relid = relid,.params = params,.in = in,.n = n,.bound = bound};

	bounds_of(&c, clause);
}
