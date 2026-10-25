/*
 * agg/accum.c - accumulation of the mapped, PostgreSQL-transition and direct aggregates
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* the functions of aggregate aggfn, whose result is of type restype, and the state the transition function starts from */
void
pgagg_lookup(PgAgg *pa, Oid aggfn, Oid restype)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfn));
	Form_pg_aggregate ag;
	bool		isnull;
	Datum		init;

	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for aggregate %u", aggfn);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	fmgr_info(ag->aggtransfn, &pa->trans);
	pa->strict = pa->trans.fn_strict;
	pa->has_final = OidIsValid(ag->aggfinalfn);
	if (pa->has_final)
		fmgr_info(ag->aggfinalfn, &pa->final);
	pa->final_extra = ag->aggfinalextra;
	get_typlenbyval(ag->aggtranstype, &pa->tlen, &pa->tbyval);
	get_typlenbyval(restype, &pa->rlen, &pa->rbyval);
	init = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_agginitval, &isnull);
	pa->init_null = isnull;
	if (!isnull)
	{
		Oid			typinput,
					typioparam;

		getTypeInputInfo(ag->aggtranstype, &typinput, &typioparam);
		pa->init = OidInputFunctionCall(typinput, TextDatumGetCString(init), typioparam, -1);
	}
	ReleaseSysCache(tup);
}

/* the context the transition functions of PostgreSQL's aggregates see (AggCheckCallContext); their states live in its curaggcontext's memory */
AggState *
pgagg_state(void)
{
	AggState   *st = makeNode(AggState);

	st->curaggcontext = CreateStandaloneExprContext();
	return st;
}

/* the lookup of the aggregate's functions, once per scan */
void
pgagg_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->pgagg = palloc0(Max(s->naggs, 1) * sizeof(PgAgg *));
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		if (a->kind != SAA_M_PG)
			continue;
		if (ex->pgstate == NULL)
			ex->pgstate = pgagg_state();
		ex->pgagg[ai] = palloc0(sizeof(PgAgg));
		pgagg_lookup(ex->pgagg[ai], a->aggfn, a->restype);
	}
}

/*
 * The state (*has: it is not NULL) gets one more value: the transition function as nodeAgg calls it (strictness, initial value, the state
 * copied out of the call's memory, callcxt, which is reset after the call).
 */
void
pgagg_step(const PgAgg *pa, AggState *st, Oid collid, MemoryContext callcxt, Datum *state, bool *has, Datum val)
{
	MemoryContext aggcxt = st->curaggcontext->ecxt_per_tuple_memory;
	LOCAL_FCINFO(fc, 2);
	Datum		r;
	MemoryContext old;

	if (!*has && !pa->init_null)
	{
		old = MemoryContextSwitchTo(aggcxt);
		*state = datumCopy(pa->init, pa->tbyval, pa->tlen);
		MemoryContextSwitchTo(old);
		*has = true;
	}
	else if (!*has && pa->strict)
	{
		old = MemoryContextSwitchTo(aggcxt);
		*state = datumCopy(val, pa->tbyval, pa->tlen);
		MemoryContextSwitchTo(old);
		*has = true;
		return;
	}
	InitFunctionCallInfoData(*fc, (FmgrInfo *) &pa->trans, 2, collid, (Node *) st, NULL);
	fc->args[0].value = *state;
	fc->args[0].isnull = !*has;
	fc->args[1].value = val;
	fc->args[1].isnull = false;
	old = MemoryContextSwitchTo(callcxt);
	r = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(aggcxt);
	if (fc->isnull)
	{
		*has = false;
		*state = (Datum) 0;
	}
	else if (pa->tbyval)
	{
		*state = r;
		*has = true;
	}
	else if (DatumGetPointer(r) != DatumGetPointer(*state))
	{
		Datum		prev = *state;
		bool		had = *has;

		*state = datumCopy(r, false, pa->tlen);
		*has = true;
		if (had)
			pfree(DatumGetPointer(prev));
	}
	else
		*has = true;
	MemoryContextSwitchTo(old);
	MemoryContextReset(callcxt);
}

/* the state becomes the result: the final function on it (the state itself without one), copied to resultcxt; *has says whether it is not NULL */
void
pgagg_result(const PgAgg *pa, AggState *st, Oid collid, MemoryContext callcxt, MemoryContext resultcxt, Datum *state, bool *has)
{
	LOCAL_FCINFO(fc, 2);
	Datum		r;
	bool		isnull;
	MemoryContext old;

	if (!pa->has_final)
		return;
	if (pa->final.fn_strict && !*has)
	{
		*state = (Datum) 0;
		return;
	}
	InitFunctionCallInfoData(*fc, (FmgrInfo *) &pa->final, pa->final_extra ? 2 : 1, collid, (Node *) st, NULL);
	fc->args[0].value = *state;
	fc->args[0].isnull = !*has;
	fc->args[1].value = (Datum) 0;
	fc->args[1].isnull = true;
	old = MemoryContextSwitchTo(callcxt);
	r = FunctionCallInvoke(fc);
	isnull = fc->isnull;
	MemoryContextSwitchTo(resultcxt);
	*state = isnull ? (Datum) 0 : datumCopy(r, pa->rbyval, pa->rlen);
	*has = !isnull;
	MemoryContextSwitchTo(old);
	MemoryContextReset(callcxt);
}

/* the state of group g gets `times` more occurrences of val */
static void
pgagg_add(AggExec *ex, int ai, uint32 g, Datum val, int64 times)
{
	AggAcc	   *acc = &ex->acc[ai];

	for (int64 t = 0; t < times; t++)
	{
		bool		has = acc->nf[g];

		pgagg_step(ex->pgagg[ai], ex->pgstate, ex->spec->agg[ai].collid, ex->comcxt, &acc->v[g], &has, val);
		acc->nf[g] = has;
	}
}

/* the result of group g: the final function on its state (the state itself without one), kept in acc->v; acc->nf says whether it is not NULL */
static void
pgagg_final(AggExec *ex, int ai, uint32 g)
{
	AggAcc	   *acc = &ex->acc[ai];
	bool		has = acc->nf[g];

	pgagg_result(ex->pgagg[ai], ex->pgstate, ex->spec->agg[ai].collid, ex->comcxt, ex->tcxt, &acc->v[g], &has);
	acc->nf[g] = has;
}

/* `times` rows of group g had the value id: min, max, sum or avg of the aggregate ai */
static void
mapped_add(AggExec *ex, int ai, uint32 g, uint32 id, int64 times)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	Datum		val = ex->aot[ai].val[id - 1];

	switch (a->kind)
	{
		case SAA_M_PG:
			pgagg_add(ex, ai, g, val, times);
			acc->n[g] += times;
			break;
		case SAA_M_MIN:
		case SAA_M_MAX:
			{
				FmgrInfo   *cmp = &lookup_type_cache(a->argtype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
				int32		c = acc->n[g] ? DatumGetInt32(FunctionCall2Coll(cmp, a->collid, val, acc->v[g])) : 0;

				if (acc->n[g] == 0 || (a->kind == SAA_M_MIN ? c < 0 : c > 0))
					acc->v[g] = val;
				acc->n[g] = 1;
			}
			break;
		default:
			if (a->restype == NUMERICOID)
			{
				Datum		sum = DirectFunctionCall2(numeric_mul, DirectFunctionCall1(int8_numeric, Int64GetDatum(times)), val);

				acc->v[g] = acc->n[g] ? DirectFunctionCall2(numeric_add, acc->v[g], sum) : sum;
			}
			else
			{
				double		x = DatumGetFloat8(val);

				acc->d[g] += times * x;
				if (!isfinite(x))
					acc->nf[g] = 1;
				else
					ex->amax[ai] = Max(ex->amax[ai], fabs(x));
			}
			acc->n[g] += times;
	}
}

/* the results of the aggregates over distinct values, from the pairs of their sets */
void
mapped_finish(AggExec *ex)
{
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		const DSet *d = &ex->dset[ai];

		if ((a->kind >= SAA_M_MIN && a->kind <= SAA_M_AVG) || a->kind == SAA_M_PG)
			for (uint64 i = 0; i < d->cap; i++)
				if (d->slot[i])
					mapped_add(ex, ai, (d->slot[i] >> 32) & 0x7fffffff, (uint32) d->slot[i], d->counted ? d->cnt[i] : 1);
		for (uint32 g = 0; a->kind == SAA_M_PG && g < (ex->hash ? ex->ngroups : ex->gcap); g++)
			if (ex->acc[ai].n[g] > 0)
				pgagg_final(ex, ai, g);
		if ((a->kind == SAA_M_AVG || a->kind == SAA_X_AVG) && a->restype == FLOAT8OID &&
			8.0 * ex->n_tuples * ex->amax[ai] * ex->amax[ai] >= DBL_MAX)
			float_overflow_error();
	}
	MemoryContextSwitchTo(old);
}

/* the sum of group g of a SAA_SUM_NUM / SAA_AVG_NUM at scale: w + i * 2^128 (agg_sum_add), as the numeric PostgreSQL's numeric_add would end with */
Datum
agg_sum_datum(const AggAcc *acc, uint32 g, int scale)
{
	Datum		lo = acc->nf[g] ? scaled_numeric(SIFT_NX_NAN, scale) : scaled_numeric_exact(acc->w[g], scale);

	if (acc->nf[g] || acc->i[g] == 0)
		return lo;
	/* 2^128 does not fit a signed 128-bit integer: 4 * 2^126 does */
	return DirectFunctionCall2(numeric_add, lo,
							   DirectFunctionCall2(numeric_mul, DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->i[g] * 4)),
												   scaled_numeric((sift_i128) 1 << 126, scale)));
}

Datum
mapped_sum_datum(AggExec *ex, const SiftAggAgg *a, int ai, uint32 g, bool *isnull)
{
	AggAcc	   *acc = &ex->acc[ai];
	bool		sum = a->kind == SAA_M_SUM || a->kind == SAA_X_SUM;

	*isnull = acc->n[g] == 0;
	if (*isnull)
		return (Datum) 0;
	if (a->restype == NUMERICOID)
	{
		Datum		tot = acc->nf[g] || a->kind == SAA_M_SUM || a->kind == SAA_M_AVG ? acc->v[g] : scaled_numeric_exact(acc->w[g], (int) acc->i[g]);

		return sum ? tot : DirectFunctionCall2(numeric_div, tot, DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
	}
	if (isinf(acc->d[g]) && !acc->nf[g])
		float_overflow_error();
	return Float8GetDatum(sum ? acc->d[g] : acc->d[g] / (double) acc->n[g]);
}

/* GROUPING(args) in a group of grouping set s: bit i (the first argument is the highest) is 1 when argument i is not grouped by the set */
int32
grouping_bits(AggExec *ex, const SiftAggAgg *a, uint32 g)
{
	uint32		mask = ex->spec->setmask[key_id(ex, ex->spec->nkeys - 1, g) - 1];
	int32		bits = 0;

	for (int i = 0; i < a->nref; i++)
		bits = bits << 1 | !(mask >> a->ref[i] & 1);
	return bits;
}
