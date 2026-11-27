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

#include "agg/ext.h"
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

void
sift_v6_bind_params(SiftV6Spec *spec, ParamListInfo params)
{
	List	   *pgexpr = NIL;

	spec->vqual = (List *) bind_param((Node *) spec->vqual, params);
	spec->filter = (List *) bind_param((Node *) spec->filter, params);
	spec->topk_count = (List *) bind_param((Node *) spec->topk_count, params);
	foreach_node(List, item, spec->pgexpr)
		pgexpr = lappend(pgexpr, list_make2(bind_param(linitial(item), params), lsecond(item)));
	spec->pgexpr = pgexpr;
	if (spec->ext == NULL)
		return;
	spec->ext->where = (List *) bind_param((Node *) spec->ext->where, params);
	spec->ext->heap_where = (List *) bind_param((Node *) spec->ext->heap_where, params);
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

static bool
bound_add(BoundCtx *c, const Var *var, bool has_lo, int64 lo, bool has_hi, int64 hi)
{
	int			slot;

	if (*c->n == SIFT_V6_MAX_BOUNDS || var->varno != c->relid || var->varlevelsup != 0 || var->varattno <= 0 || !bound_type(var->vartype) ||
		(slot = sift_v6_input_add_var(c->in, var)) < 0 || c->in->col[slot].scale != 0)
		return false;
	c->bound[(*c->n)++] = (SiftV6Bound) {.slot = slot,.has_lo = has_lo,.lo = lo,.has_hi = has_hi,.hi = hi};
	return true;
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

/* ---- the ranges of the disjuncts of an OR ---- */

/* no value lies in every bound the range puts on one of its columns */
static bool
v6_range_empty(const SiftV6Range *r)
{
	for (int i = 0; i < r->nbounds; i++)
		for (int j = 0; j < r->nbounds; j++)
			if (r->bound[i].slot == r->bound[j].slot && r->bound[i].has_lo && r->bound[j].has_hi && r->bound[i].lo > r->bound[j].hi)
				return true;
	return false;
}

static bool
range_add(SiftV6RangeSet *out, const SiftV6Range *r)
{
	if (r->nbounds == 0 || out->nranges == SIFT_V6_MAX_RANGES)
		return false;
	if (!v6_range_empty(r))
		out->range[out->nranges++] = *r;
	return true;
}

/* `col = ANY (array)`: a range for each value while they fit, else one from the least to the greatest */
static bool
ranges_of_any(BoundCtx *c, const ScalarArrayOpExpr *sa, SiftV6RangeSet *out)
{
	const char *name = get_opname(sa->opno);
	const Var  *var = linitial(sa->args);
	const int64 *vals;
	int			n;
	SiftV6Range r = {0};

	if (!sa->useOr || name == NULL || strcmp(name, "=") != 0 || !IsA(var, Var) || !bound_type(var->vartype) ||
		!sift_v6_array_ints(CurrentMemoryContext, lsecond(sa->args), var->vartype, c->params, &vals, &n))
		return false;
	if (out->nranges + n > SIFT_V6_MAX_RANGES)
	{
		c->n = &r.nbounds;
		c->bound = r.bound;
		bound_of_any(c, sa);
		return range_add(out, &r);
	}
	for (int i = 0; i < n; i++)
	{
		r.nbounds = 0;
		c->n = &r.nbounds;
		c->bound = r.bound;
		bound_add(c, var, true, vals[i], true, vals[i]);
		if (!range_add(out, &r))
			return false;
	}
	return true;
}

/* the bounds of a conjunct of a disjunct: IS NULL of a column too, which a zone with no NULL rules out */
static void
range_bounds_of(BoundCtx *c, const Node *clause)
{
	const NullTest *nt = (const NullTest *) clause;
	const Var  *var = (const Var *) nt->arg;

	if (!IsA(clause, NullTest) || nt->nulltesttype != IS_NULL || nt->argisrow || !IsA(var, Var))
	{
		bounds_of(c, clause);
		return;
	}
	if (bound_add(c, var, false, 0, false, 0))
		c->bound[*c->n - 1].isnull = true;
}

static bool
ranges_of(BoundCtx *c, const Node *d, SiftV6RangeSet *out)
{
	SiftV6Range r = {0};

	if (IsA(d, RestrictInfo))
		d = (const Node *) ((const RestrictInfo *) d)->clause;
	if (is_orclause(d))
	{
		foreach_ptr(Node, arg, ((const BoolExpr *) d)->args)
			if (!ranges_of(c, arg, out))
				return false;
		return true;
	}
	if (IsA(d, ScalarArrayOpExpr))
		return ranges_of_any(c, (const ScalarArrayOpExpr *) d, out);
	c->n = &r.nbounds;
	c->bound = r.bound;
	if (is_andclause(d))
		foreach_ptr(Node, arg, ((const BoolExpr *) d)->args)
			range_bounds_of(c, arg);
	else
		range_bounds_of(c, d);
	return range_add(out, &r);
}

bool
sift_v6_or_ranges_of(MemoryContext keep, const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6RangeSet *out)
{
	BoundCtx	c = {.relid = relid,.params = params,.in = in};

	if (IsA(clause, RestrictInfo))
		clause = (const Node *) ((const RestrictInfo *) clause)->clause;
	if (!is_orclause(clause))
		return false;
	*out = (SiftV6RangeSet) {.range = MemoryContextAlloc(keep, SIFT_V6_MAX_RANGES * sizeof(SiftV6Range))};
	return ranges_of(&c, clause, out);
}

bool
sift_v6_ranges_of(MemoryContext keep, const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6RangeSet *out)
{
	if (IsA(clause, RestrictInfo))
		clause = (const Node *) ((const RestrictInfo *) clause)->clause;
	return sift_v6_or_ranges_of(keep, clause, relid, params, in, out) || sift_v6_periodic_ranges(keep, clause, relid, params, in, out);
}
