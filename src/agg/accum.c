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

/* the lookup of the aggregate's functions, once per scan */
void
pgagg_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->pgagg = palloc0(Max(s->naggs, 1) * sizeof(PgAgg *));
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];
		HeapTuple	tup;
		Form_pg_aggregate ag;
		PgAgg	   *pa;
		bool		isnull;
		Datum		init;

		if (a->kind != SAA_M_PG)
			continue;
		if (ex->pgstate == NULL)
		{
			ex->pgstate = makeNode(AggState);
			ex->pgstate->curaggcontext = CreateStandaloneExprContext();
		}
		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfn));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for aggregate %u", a->aggfn);
		ag = (Form_pg_aggregate) GETSTRUCT(tup);
		pa = ex->pgagg[ai] = palloc0(sizeof(PgAgg));
		fmgr_info(ag->aggtransfn, &pa->trans);
		pa->strict = pa->trans.fn_strict;
		pa->has_final = OidIsValid(ag->aggfinalfn);
		if (pa->has_final)
			fmgr_info(ag->aggfinalfn, &pa->final);
		pa->final_extra = ag->aggfinalextra;
		get_typlenbyval(ag->aggtranstype, &pa->tlen, &pa->tbyval);
		get_typlenbyval(a->restype, &pa->rlen, &pa->rbyval);
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
}

/* the state of group g gets `times` more occurrences of val: the transition function as nodeAgg calls it (strictness, initial value, the state copied out of the call's memory) */
static void
pgagg_add(AggExec *ex, int ai, uint32 g, Datum val, int64 times)
{
	PgAgg	   *pa = ex->pgagg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	MemoryContext aggcxt = ex->pgstate->curaggcontext->ecxt_per_tuple_memory;
	LOCAL_FCINFO(fc, 2);

	for (int64 t = 0; t < times; t++)
	{
		Datum		r;
		MemoryContext old;

		if (!acc->nf[g] && !pa->init_null)
		{
			old = MemoryContextSwitchTo(aggcxt);
			acc->v[g] = datumCopy(pa->init, pa->tbyval, pa->tlen);
			MemoryContextSwitchTo(old);
			acc->nf[g] = 1;
		}
		else if (!acc->nf[g] && pa->strict)
		{
			old = MemoryContextSwitchTo(aggcxt);
			acc->v[g] = datumCopy(val, pa->tbyval, pa->tlen);
			MemoryContextSwitchTo(old);
			acc->nf[g] = 1;
			continue;
		}
		InitFunctionCallInfoData(*fc, &pa->trans, 2, ex->spec->agg[ai].collid, (Node *) ex->pgstate, NULL);
		fc->args[0].value = acc->v[g];
		fc->args[0].isnull = !acc->nf[g];
		fc->args[1].value = val;
		fc->args[1].isnull = false;
		old = MemoryContextSwitchTo(ex->comcxt);
		r = FunctionCallInvoke(fc);
		MemoryContextSwitchTo(aggcxt);
		if (fc->isnull)
		{
			acc->nf[g] = 0;
			acc->v[g] = (Datum) 0;
		}
		else if (pa->tbyval)
		{
			acc->v[g] = r;
			acc->nf[g] = 1;
		}
		else if (DatumGetPointer(r) != DatumGetPointer(acc->v[g]))
		{
			Datum		prev = acc->v[g];
			bool		had = acc->nf[g];

			acc->v[g] = datumCopy(r, false, pa->tlen);
			acc->nf[g] = 1;
			if (had)
				pfree(DatumGetPointer(prev));
		}
		else
			acc->nf[g] = 1;
		MemoryContextSwitchTo(old);
		MemoryContextReset(ex->comcxt);
	}
}

/* the result of group g: the final function on its state (the state itself without one), kept in acc->v; acc->nf says whether it is not NULL */
static void
pgagg_final(AggExec *ex, int ai, uint32 g)
{
	PgAgg	   *pa = ex->pgagg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	LOCAL_FCINFO(fc, 2);
	Datum		r;
	bool		isnull;
	MemoryContext old;

	if (!pa->has_final)
		return;
	if (pa->final.fn_strict && !acc->nf[g])
	{
		acc->v[g] = (Datum) 0;
		return;
	}
	InitFunctionCallInfoData(*fc, &pa->final, pa->final_extra ? 2 : 1, ex->spec->agg[ai].collid, (Node *) ex->pgstate, NULL);
	fc->args[0].value = acc->v[g];
	fc->args[0].isnull = !acc->nf[g];
	fc->args[1].value = (Datum) 0;
	fc->args[1].isnull = true;
	old = MemoryContextSwitchTo(ex->comcxt);
	r = FunctionCallInvoke(fc);
	isnull = fc->isnull;
	MemoryContextSwitchTo(ex->tcxt);
	acc->v[g] = isnull ? (Datum) 0 : datumCopy(r, pa->rbyval, pa->rlen);
	acc->nf[g] = !isnull;
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
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
		Datum		tot = acc->nf[g] || a->kind == SAA_M_SUM || a->kind == SAA_M_AVG ? acc->v[g] : scaled_numeric(acc->w[g], (int) acc->i[g]);

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
