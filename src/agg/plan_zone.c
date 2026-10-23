/*
 * agg/plan_zone.c - the share of the units the zone tests of a query leave, by the zones of the source's units
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/colsrc.h"
#include "agg/spec.h"
#include "agg/plan_int.h"
#include "core/cmp.h"

/* The values a comparison or IN list keeps, as the executor reads them (prune_init): the range [lo, hi] and, for a set, its sorted values */
typedef struct ZoneRange
{
	int64		lo;
	int64		hi;
	int			nvals;
	int64	   *vals;
} ZoneRange;

/* the constants of an array operand: the elements of a Const array, or of an ArrayExpr whose elements are all Consts */
static bool
array_consts(Node *e, Oid elemtype, Datum **vals, bool **nulls, int *n)
{
	if (e && IsA(e, Const) && !((Const *) e)->constisnull)
	{
		int16		len;
		bool		byval;
		char		align;

		get_typlenbyvalalign(elemtype, &len, &byval, &align);
		deconstruct_array(DatumGetArrayTypeP(((Const *) e)->constvalue), elemtype, len, byval, align, vals, nulls, n);
		return true;
	}
	if (e && IsA(e, ArrayExpr))
	{
		List	   *els = ((ArrayExpr *) e)->elements;
		int			i = 0;

		*vals = palloc(Max(list_length(els), 1) * sizeof(Datum));
		*nulls = palloc(Max(list_length(els), 1) * sizeof(bool));
		foreach_ptr(Node, el, els)
		{
			if (!IsA(el, Const))
				return false;
			(*vals)[i] = ((Const *) el)->constvalue;
			(*nulls)[i++] = ((Const *) el)->constisnull;
		}
		*n = i;
		return true;
	}
	return false;
}

/* The range of the values a conjunct of a unit's zone keeps, from its operand's value at plan time; false when the operand is not a constant (a parameter of a generic plan) or the leaf is not an integer */
static bool
zone_range(AggCtx *c, const SiftAggPred *p, ZoneRange *r)
{
	Node	   *e = estimate_expression_value(c->root, (Node *) p->expr);
	const SiftSrcLeaf *lf = &c->lay->leaf[p->leaf];

	*r = (ZoneRange) {PG_INT64_MIN, PG_INT64_MAX};
	if (p->argtype == NUMERICOID || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64))
		return false;
	if (p->kind == SAP_CMP)
	{
		SiftCmpOp	op = (SiftCmpOp) p->op;
		int64		v;

		if (!IsA(e, Const) || ((Const *) e)->constisnull)
			return false;
		v = sift_aggspec_operand_int(p->argtype, ((Const *) e)->constvalue);
		if (op != SCMP_GT && op != SCMP_GE)
			r->hi = op == SCMP_LT && v > PG_INT64_MIN ? v - 1 : v;
		if (op != SCMP_LT && op != SCMP_LE)
			r->lo = op == SCMP_GT && v < PG_INT64_MAX ? v + 1 : v;
		return true;
	}
	else
	{
		Datum	   *el;
		bool	   *nl;
		int			ne,
					nv = 0;

		if (!array_consts(e, p->argtype, &el, &nl, &ne))
			return false;
		r->vals = palloc(Max(ne, 1) * sizeof(int64));
		for (int i = 0; i < ne; i++)
		{
			int64		v;

			if (nl[i])
				continue;
			v = sift_aggspec_operand_int(p->argtype, el[i]);
			if (lf->vtype != SCV_I64 && (v < PG_INT32_MIN || v > PG_INT32_MAX))
				continue;
			r->vals[nv++] = v;
		}
		qsort(r->vals, nv, sizeof(int64), sift_cmp_i64);
		r->nvals = nv;
		r->lo = nv > 0 ? r->vals[0] : PG_INT64_MAX;
		r->hi = nv > 0 ? r->vals[nv - 1] : PG_INT64_MIN;
		return true;
	}
}

/*
 * the bytes the scan clears to test the IN list p when it starts (agg/pred_in.c); 0 for an empty list, a list that is not constants (but see below) or a hash
 * table. An array the plan cannot see (a parameter of a generic plan) has sift.agg_param_set_size values, taken over the whole range of the leaf: the keys of
 * a list are spread over it unless the query says otherwise, and a plan that is made once must not assume the narrow one.
 */
double
in_list_table_bytes(AggCtx *c, const SiftAggPred *p)
{
	ZoneRange	r;
	const SiftSrcLeaf *lf;

	if (p->kind != SAP_IN)
		return 0;
	if (zone_range(c, p, &r))
		return r.nvals == 0 ? 0 : sift_agg_in_table_bytes((uint64) r.hi - (uint64) r.lo + 1, r.nvals);
	lf = &c->lay->leaf[p->leaf];
	if (p->argtype == NUMERICOID || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64) || sift_agg_param_set_size <= 0 ||
		!IsA(estimate_expression_value(c->root, (Node *) p->expr), Param) || c->stats.max[p->leaf] <= c->stats.min[p->leaf])
		return 0;
	return sift_agg_in_table_bytes((uint64) (c->stats.max[p->leaf] - c->stats.min[p->leaf]) + 1, sift_agg_param_set_size);
}

/* may a unit whose zone is [zlo, zhi] hold a value the range keeps? */
static bool
zone_meets(const ZoneRange *r, int64 zlo, int64 zhi)
{
	int			a = 0,
				b = r->nvals;

	if (zlo > zhi || zhi < r->lo || zlo > r->hi)
		return false;
	if (r->nvals == 0)
		return true;
	/* the first value of the set that is not below the zone: is it within it */
	while (a < b)
	{
		int			m = a + (b - a) / 2;

		if (r->vals[m] < zlo)
			a = m + 1;
		else
			b = m;
	}
	return a < r->nvals && r->vals[a] <= zhi;
}

/* the range of a comparison or IN list of a unit's zone, by the node's number: the constant is read once */
typedef struct ZoneTest
{
	bool		done;
	bool		ok;
	ZoneRange	r;
} ZoneTest;

/* can the zones of the sample tell whether a unit may hold a row p keeps: a comparison with a constant, an AND of which one conjunct can, an OR of which all disjuncts can */
static bool
zone_testable(AggCtx *c, const SiftSrcZones *z, const SiftAggPred *p, ZoneTest *t)
{
	if (p->kind == SAP_AND || p->kind == SAP_OR)
	{
		bool		any = false,
					all = true;

		for (int i = 0; i < p->nchild; i++)
		{
			bool		ok = zone_testable(c, z, p->child[i], t);

			any |= ok;
			all &= ok;
		}
		return p->kind == SAP_AND ? any : all && p->nchild > 0;
	}
	if (!t[p->id].done)
	{
		t[p->id].done = true;
		t[p->id].ok = sift_aggspec_zone_cmp(p, c->lay, c->lay->nleaves) && z->lo[p->leaf] != NULL && zone_range(c, p, &t[p->id].r);
	}
	return t[p->id].ok;
}

/* may unit k hold a row p keeps (an untestable node may) */
static bool
zone_may(const SiftSrcZones *z, const SiftAggPred *p, ZoneTest *t, int k)
{
	if (p->kind == SAP_AND)
	{
		for (int i = 0; i < p->nchild; i++)
			if (!zone_may(z, p->child[i], t, k))
				return false;
		return true;
	}
	if (p->kind == SAP_OR)
	{
		for (int i = 0; i < p->nchild; i++)
			if (zone_may(z, p->child[i], t, k))
				return true;
		return false;
	}
	return !t[p->id].ok || zone_meets(&t[p->id].r, z->lo[p->leaf][k], z->hi[p->leaf][k]);
}

/*
 * The share of the units the conjuncts of the query leave, counted on the sample of the real zones of the source's units: a unit stays when
 * every conjunct may hold a row in it (a comparison or IN list with a constant by the zone of its leaf, an OR when one of its disjuncts
 * may), tested together, so two windows that no unit meets leave about none. The units the sample does not describe (a write buffer) stay.
 * The conjuncts it cannot test (no zones in the source, a parameter, a numeric, a ctid range) go to *rest for the estimate by selectivity.
 * 1.0 when none could be tested.
 */
double
zone_sample_kept(AggCtx *c, SiftAggSpec *s, List **rest)
{
	const SiftSrcZones *z = c->stats.zones;
	ZoneTest   *t;
	List	   *tested = NIL;
	bool	   *alive;
	int			left = 0;
	double		share;

	*rest = NIL;
	if (z == NULL)
	{
		for (int i = 0; i < s->npred; i++)
			*rest = lappend(*rest, s->pred[i]);
		return 1.0;
	}
	t = palloc0((c->npred + 1) * sizeof(ZoneTest));
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind != SAP_TID && zone_testable(c, z, s->pred[i], t))
			tested = lappend(tested, s->pred[i]);
		else
			*rest = lappend(*rest, s->pred[i]);
	if (tested == NIL)
		return 1.0;
	alive = palloc(z->n * sizeof(bool));
	for (int k = 0; k < z->n; k++)
	{
		alive[k] = true;
		foreach_ptr(SiftAggPred, p, tested)
			alive[k] = alive[k] && zone_may(z, p, t, k);
		left += alive[k];
	}
	share = (double) left / z->n;
	return z->covered * share + (1.0 - z->covered);
}
