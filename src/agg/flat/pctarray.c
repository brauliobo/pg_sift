/*
 * agg/flat/pctarray.c - the percentiles of a constant array of fractions are the array of the percentile of each fraction
 * `percentile_cont(array[0.25, 0.5, 0.75]) WITHIN GROUP (ORDER BY x)` is `array[percentile_cont(0.25) .. , percentile_cont(0.5) .. , percentile_cont(0.75) ..]`: the
 * aggregates sort the same values, so the scan keeps them once (agg/osa.c) and picks the ranks of every fraction from them, where the normal plan feeds PostgreSQL's
 * own transition function, which sorts all the values of the group in a tuplesort. A NULL fraction is a NULL element, a group with no value is a NULL, not an array of NULLs
 * (the first percentile is NULL then, and only then), and the dimension and lower bound of the result are those of the fractions: a one-dimensional array that starts at 1 and
 * has fractions in [0, 1] (the others raise their errors in the normal plan). The planner hook plans the query both ways and keeps this one if it runs in the scan and costs less.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

#define MAX_FRACTIONS 64

static bool
percentile_array(Aggref *a)
{
	return a->agglevelsup == 0 && (a->aggfnoid == F_PERCENTILE_CONT__FLOAT8_FLOAT8 || a->aggfnoid == F_PERCENTILE_DISC__FLOAT8_ANYELEMENT) && a->aggfilter == NULL &&
		list_length(a->aggdirectargs) == 1 && list_length(a->args) == 1 && list_length(a->aggorder) == 1 && a->aggdistinct == NIL;
}

/* the fractions as constants (NULL for a NULL element), or NIL when the array is not a constant one-dimensional array from 1, empty, long, or with a fraction outside [0, 1] */
static List *
fractions(Node *arg)
{
	List	   *r = NIL;

	arg = eval_const_expressions(NULL, arg);
	if (IsA(arg, ArrayExpr) && !((ArrayExpr *) arg)->multidims)
	{
		foreach_ptr(Node, e, ((ArrayExpr *) arg)->elements)
		{
			e = eval_const_expressions(NULL, e);
			if (!IsA(e, Const))
				return NIL;
			r = lappend(r, e);
		}
	}
	else if (IsA(arg, Const) && !((Const *) arg)->constisnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(((Const *) arg)->constvalue);
		Datum	   *vals;
		bool	   *nulls;
		int			n;

		if (ARR_NDIM(arr) != 1 || ARR_LBOUND(arr)[0] != 1 || ARR_ELEMTYPE(arr) != FLOAT8OID)
			return NIL;
		deconstruct_array(arr, FLOAT8OID, sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &vals, &nulls, &n);
		for (int i = 0; i < n; i++)
			r = lappend(r, makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8), vals[i], nulls[i], FLOAT8PASSBYVAL));
	}
	else
		return NIL;
	if (r == NIL || list_length(r) > MAX_FRACTIONS)
		return NIL;
	foreach_ptr(Const, c, r)
	{
		double		f;

		if (c->consttype != FLOAT8OID)
			return NIL;
		f = c->constisnull ? 0.5 : DatumGetFloat8(c->constvalue);
		if (!(f >= 0 && f <= 1))
			return NIL;
	}
	return r;
}

static bool
candidate_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref) && percentile_array((Aggref *) n) && fractions(linitial(((Aggref *) n)->aggdirectargs)) != NIL)
		return true;
	return expression_tree_walker(n, candidate_walker, context);
}

/* a query with such an aggregate in its select list or HAVING (the test that costs no copy of the query) */
bool
sift_agg_pct_array_candidate(Query *q)
{
	return q->commandType == CMD_SELECT && q->hasAggs && (candidate_walker((Node *) q->targetList, NULL) || candidate_walker(q->havingQual, NULL));
}

/* the aggregate for one fraction: the array aggregate's own arguments and sort order, the scalar function of its kind */
static Aggref *
scalar_aggregate(Aggref *a, Const *frac, Oid elemtype)
{
	Aggref	   *s = copyObject(a);

	s->aggfnoid = a->aggfnoid == F_PERCENTILE_CONT__FLOAT8_FLOAT8 ? F_PERCENTILE_CONT_FLOAT8_FLOAT8 : F_PERCENTILE_DISC_FLOAT8_ANYELEMENT;
	s->aggtype = elemtype;
	s->aggdirectargs = list_make1(frac);
	s->aggargtypes = list_make2_oid(FLOAT8OID, exprType((Node *) linitial_node(TargetEntry, a->args)->expr));
	return s;
}

static Node *
array_mutator(Node *n, bool *changed)
{
	Aggref	   *a;
	List	   *fracs,
			   *elems = NIL;
	Oid			elemtype;
	Aggref	   *first = NULL;
	ArrayExpr  *arr;
	CaseExpr   *c;
	CaseWhen   *w;
	NullTest   *t;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Aggref) || !percentile_array((Aggref *) n))
		return expression_tree_mutator(n, array_mutator, changed);
	a = (Aggref *) n;
	elemtype = get_element_type(a->aggtype);
	fracs = fractions(linitial(a->aggdirectargs));
	if (fracs == NIL || !OidIsValid(elemtype))
		return n;
	foreach_ptr(Const, f, fracs)
	{
		if (f->constisnull)
			elems = lappend(elems, makeNullConst(elemtype, -1, InvalidOid));
		else
		{
			Aggref	   *s = scalar_aggregate(a, f, elemtype);

			first = first == NULL ? s : first;
			elems = lappend(elems, s);
		}
	}
	if (first == NULL)
		return n;
	arr = makeNode(ArrayExpr);
	arr->array_typeid = a->aggtype;
	arr->element_typeid = elemtype;
	arr->elements = elems;
	arr->location = -1;
	t = makeNode(NullTest);
	t->arg = (Expr *) copyObject(first);
	t->nulltesttype = IS_NULL;
	t->location = -1;
	w = makeNode(CaseWhen);
	w->expr = (Expr *) t;
	w->result = (Expr *) makeNullConst(a->aggtype, -1, InvalidOid);
	w->location = -1;
	c = makeNode(CaseExpr);
	c->casetype = a->aggtype;
	c->args = list_make1(w);
	c->defresult = (Expr *) arr;
	c->location = -1;
	*changed = true;
	return (Node *) c;
}

bool
sift_agg_pct_arrays(Query *q, bool apply)
{
	bool		changed = false;

	if (!sift_agg_pct_array_candidate(q))
		return false;
	if (!apply)
		return true;
	q->targetList = (List *) array_mutator((Node *) q->targetList, &changed);
	q->havingQual = array_mutator(q->havingQual, &changed);
	if (changed)
		sift_agg_by_cost = sift_agg_flat_any = true;
	return changed;
}
