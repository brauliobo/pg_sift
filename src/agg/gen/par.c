/*
 * agg/gen/par.c - SV6_GEN_PG in workers: the partial state of a group as the bytes of a parallel record (the serial function's, or a serialized datum), and the
 * test of which aggregates can run there at all
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/datum.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

#include "core/catname.h"
#include "core/compat.h"
#include "agg/gen/gen.h"
#include "agg/osa/ship.h"

/* ---- the partial states of a parallel run ---- */

static bool
carries_value(const V6Acc *acc)
{
	return (acc->n & V6GEN_INIT) && !(acc->n & V6GEN_NULL);
}

void
v6_gen_state_put(V6Gen *gen, int j, const V6Acc *acc, StringInfo buf)
{
	V6GenAgg   *ga = gen->agg[j];
	MemoryContext old;
	uint32		len;

	if (ga->sorted)
	{
		if (acc->v1 != 0)
			v6_osa_put(ga, (uint32) acc->v1 - 1, buf);
		return;
	}
	if (!carries_value(acc))
		return;
	old = MemoryContextSwitchTo(gen->tmp->ecxt_per_tuple_memory);
	if (ga->sfc != NULL)
	{
		bytea	   *r;

		ga->sfc->args[0].value = (Datum) acc->v0;
		ga->sfc->args[0].isnull = false;
		ga->sfc->isnull = false;
		v6_gen_call_context(gen, ga, false);
		r = DatumGetByteaPP(FunctionCallInvoke(ga->sfc));
		len = VARSIZE_ANY(r);
		appendBinaryStringInfo(buf, (char *) &len, sizeof(len));
		appendBinaryStringInfo(buf, (char *) r, len);
	}
	else
	{
		char	   *at;
		const Size	sz = datumEstimateSpace((Datum) acc->v0, false, ga->tbyval, ga->tlen);

		len = (uint32) sz;
		appendBinaryStringInfo(buf, (char *) &len, sizeof(len));
		enlargeStringInfo(buf, sz);
		at = buf->data + buf->len;
		datumSerialize((Datum) acc->v0, false, ga->tbyval, ga->tlen, &at);
		buf->len += sz;
	}
	MemoryContextSwitchTo(old);
	ResetExprContext(gen->tmp);
}

const char *
v6_gen_state_take(V6Gen *gen, int j, V6Acc *dst, const V6Acc *src, const char *tail)
{
	V6GenAgg   *ga = gen->agg[j];
	V6Acc		s = *src;
	uint32		len;
	MemoryContext old;

	if (ga->sorted)
		return src->v1 != 0 ? v6_osa_take(gen, ga, dst, tail) : tail;
	if (carries_value(src))
	{
		memcpy(&len, tail, sizeof(len));
		tail += sizeof(len);
		old = MemoryContextSwitchTo(gen->tmp->ecxt_per_tuple_memory);
		if (ga->dfc != NULL)
		{
			bytea	   *r = palloc(len);

			memcpy(r, tail, len);
			ga->dfc->args[0].value = PointerGetDatum(r);
			ga->dfc->args[0].isnull = false;
			ga->dfc->args[1].value = (Datum) 0;
			ga->dfc->args[1].isnull = false;
			ga->dfc->isnull = false;
			v6_gen_call_context(gen, ga, false);
			s.v0 = (uint64) FunctionCallInvoke(ga->dfc);
		}
		else
		{
			const char *at = tail;
			bool		isnull;

			s.v0 = (uint64) datumRestore((char **) &at, &isnull);
		}
		MemoryContextSwitchTo(old);
		tail += len;
	}
	v6_gen_combine(gen, j, dst, &s);
	ResetExprContext(gen->tmp);
	return tail;
}

void
v6_gen_release(V6Gen *gen)
{
	for (int j = 0; j < gen->spec->naggs; j++)
		if (gen->agg[j] != NULL)
		{
			v6_osa_release(gen->agg[j]);
			ReScanExprContext(gen->agg[j]->cx);
		}
}

void
v6_gen_unbound(V6Gen *gen)
{
	gen->limit = UINT64_MAX;
}

/* the states that grow with the rows of a group: a record of a parallel run holds one state, and a state of these is as large as the group */
static bool
unbounded_state(const Aggref *a, Oid transtype)
{
	static const char *const growing[] = {"array_agg", "string_agg", "json_agg", "jsonb_agg", "json_agg_strict", "jsonb_agg_strict", "json_object_agg", "jsonb_object_agg",
		"json_object_agg_strict", "jsonb_object_agg_strict", "json_object_agg_unique", "jsonb_object_agg_unique", "json_object_agg_unique_strict",
		"jsonb_object_agg_unique_strict", "xmlagg", "range_agg"};
	int16		len;
	bool		byval;

	if (get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE)
	{
		const char *name = sift_func_name(a->aggfnoid);

		for (int i = 0; i < lengthof(growing); i++)
			if (strcmp(name, growing[i]) == 0)
				return true;
		return false;
	}
	get_typlenbyval(transtype, &len, &byval);
	return !byval && len <= 0;
}

/* why the partial state of the aggregate cannot be made, carried and combined, as PostgreSQL's own parallel aggregate does (NULL: it can) */
static const char *
state_serial_why(const Aggref *a, const Form_pg_aggregate ag)
{
	if (a->aggorder != NIL || a->aggkind != AGGKIND_NORMAL)
		return "the ORDER BY of its inputs keeps the values of a group in one sort";
	if (unbounded_state(a, ag->aggtranstype))
		return "its state grows with the rows of a group, and a record of the workers carries one state";
	if (!OidIsValid(ag->aggcombinefn) || IsPolymorphicType(ag->aggtranstype) || func_volatile(ag->aggcombinefn) == PROVOLATILE_VOLATILE ||
		(ag->aggtranstype == INTERNALOID && (!OidIsValid(ag->aggserialfn) || !OidIsValid(ag->aggdeserialfn))))
		return "its partial states cannot be carried and combined";
	return NULL;
}

/* why the aggregate cannot run in workers (NULL: a partial state of it can be carried and combined, or it is a WITHIN GROUP aggregate whose inputs the records carry, and its functions are parallel safe) */
static const char *
serial_aggregate(const Aggref *a)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfnoid));
	Form_pg_aggregate ag;
	const char *why = NULL;

	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for aggregate %u", a->aggfnoid);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	if (a->aggdistinct != NIL)
		why = "DISTINCT keeps the values of a group in one sort or set";
	else if (!sift_v6_osa_shippable(a))
		why = state_serial_why(a, ag);
	foreach_oid(fn, list_make5_oid(a->aggfnoid, ag->aggtransfn, ag->aggcombinefn, ag->aggserialfn, ag->aggdeserialfn))
		if (why == NULL && OidIsValid(fn) && func_parallel(fn) != PROPARALLEL_SAFE)
			why = "a function of it is not parallel safe";
	ReleaseSysCache(tup);
	return why;
}

const char *
sift_v6_gen_serial_why(PlannerInfo *root, Index rti, const Aggref *a)
{
	const Oid	relid = planner_rt_fetch(rti, root)->relid;
	const char *why = serial_aggregate(a);
	Node	   *named;

	if (why == NULL)
		return NULL;
	named = copyObjectImpl(a);
	ChangeVarNodes(named, rti, 1, 0);
	return psprintf("%s: %s", deparse_expression(named, deparse_context_for(get_rel_name(relid), relid), true, false), why);
}
