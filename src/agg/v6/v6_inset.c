/*
 * agg/v6/v6_inset.c - `column = ANY (array)` as a set of integers (v6_vexpr.h), for the arrays the machine's program cannot hold: external parameters and constants of more than a dozen values.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_vexpr.h"

static bool
int_class(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool
column_type(Oid t)
{
	return int_class(t) || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static int64
as_int(Oid t, Datum d)
{
	return t == INT2OID ? DatumGetInt16(d) : t == INT4OID || t == DATEOID ? DatumGetInt32(d) : DatumGetInt64(d);
}

static int
cmp_int64(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* the array a constant or this execution's parameter holds; false for any other node, or a NULL array */
static bool
array_of(const Node *e, ParamListInfo params, Datum *value)
{
	bool		isnull;

	if (IsA(e, Const))
	{
		*value = ((const Const *) e)->constvalue;
		return !((const Const *) e)->constisnull;
	}
	return IsA(e, Param) && sift_v6_param_value(params, (const Param *) e, value, &isnull) && !isnull;
}

bool
sift_v6_array_ints(MemoryContext keep, const Node *arr, Oid vartype, ParamListInfo params, const int64 **vals, int *n)
{
	Datum		d;
	ArrayType  *a;
	Oid			t;
	int16		len;
	bool		byval;
	char		align;
	Datum	   *el;
	bool	   *nulls;
	int			cnt,
				out = 0;
	int64	   *v;
	MemoryContext old;

	if (!OidIsValid(get_element_type(exprType(arr))) || !column_type(vartype))
		return false;
	t = get_element_type(exprType(arr));
	if (!column_type(t) || (t != vartype && !(int_class(t) && int_class(vartype))))
		return false;
	if (!array_of(arr, params, &d))
	{
		*vals = NULL;
		*n = 0;
		return IsA(arr, Param) && params == NULL;
	}
	a = DatumGetArrayTypeP(d);
	get_typlenbyvalalign(t, &len, &byval, &align);
	deconstruct_array(a, t, len, byval, align, &el, &nulls, &cnt);
	old = MemoryContextSwitchTo(keep);
	v = palloc(Max(cnt, 1) * sizeof(int64));
	MemoryContextSwitchTo(old);
	for (int i = 0; i < cnt; i++)
		if (!nulls[i])
			v[out++] = as_int(t, el[i]);
	qsort(v, out, sizeof(int64), cmp_int64);
	*vals = v;
	*n = out;
	return true;
}

bool
sift_v6_inset_of(MemoryContext keep, const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6InSet *out)
{
	const ScalarArrayOpExpr *sa = (const ScalarArrayOpExpr *) clause;
	const char *name;
	const Var  *var;
	const int64 *vals;
	int			n;

	if (IsA(clause, RestrictInfo))
		sa = (const ScalarArrayOpExpr *) ((const RestrictInfo *) clause)->clause;
	if (!IsA(sa, ScalarArrayOpExpr) || !sa->useOr || list_length(sa->args) != 2 || (name = get_opname(sa->opno)) == NULL || strcmp(name, "=") != 0)
		return false;
	var = linitial(sa->args);
	if (!IsA(var, Var) || var->varno != relid || var->varlevelsup != 0 || var->varattno <= 0 ||
		!sift_v6_array_ints(keep, lsecond(sa->args), var->vartype, params, &vals, &n))
		return false;
	out->slot = sift_v6_input_add_var(in, var);
	out->vals = vals;
	out->n = n;
	return out->slot >= 0;
}
