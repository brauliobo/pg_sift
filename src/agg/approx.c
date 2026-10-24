/*
 * agg/approx.c - sift.approx_count_distinct(x): the aggregate behind the opt-in approximate distinct count (docs/USAGE.md "Approximate distinct counts"). An
 * ordinary aggregate over any type with a hash operator class: the state is a HyperLogLog sketch (col/hll.h) of the extended hash of each value, so the
 * normal plan, the parallel plan and SiftAgg (which hashes the same way, or reads the sketches a unit stores) give the same number. count(DISTINCT) is
 * not touched by it: nothing rewrites one into the other.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/typcache.h"

#include "col/hll.h"

PG_FUNCTION_INFO_V1(sift_approx_trans);
PG_FUNCTION_INFO_V1(sift_approx_combine);
PG_FUNCTION_INFO_V1(sift_approx_serial);
PG_FUNCTION_INFO_V1(sift_approx_deserial);
PG_FUNCTION_INFO_V1(sift_approx_final);

typedef struct ApproxHash
{
	Oid			collid;
	FmgrInfo	fn;
} ApproxHash;

static MemoryContext
approx_cxt(FunctionCallInfo fcinfo)
{
	MemoryContext aggcxt;

	if (!AggCheckCallContext(fcinfo, &aggcxt))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the support functions of sift.approx_count_distinct run only inside the aggregate"), errhint("Call sift.approx_count_distinct(x).")));
	return aggcxt;
}

static SiftHll *
approx_state(FunctionCallInfo fcinfo)
{
	MemoryContext aggcxt = approx_cxt(fcinfo);
	SiftHll    *h;

	if (!PG_ARGISNULL(0))
		return (SiftHll *) PG_GETARG_POINTER(0);
	h = MemoryContextAlloc(aggcxt, sizeof(SiftHll));
	sift_hll_init(h, aggcxt);
	return h;
}

Datum
sift_approx_trans(PG_FUNCTION_ARGS)
{
	SiftHll    *h = approx_state(fcinfo);
	ApproxHash *ah = fcinfo->flinfo->fn_extra;

	if (!PG_ARGISNULL(1))
	{
		if (ah == NULL)
		{
			Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 1);
			TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_HASH_EXTENDED_PROC_FINFO);

			if (!OidIsValid(tc->hash_extended_proc))
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("could not identify an extended hash function for type %s", format_type_be(typid)),
								errhint("sift.approx_count_distinct needs a type with a hash operator class.")));
			ah = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(ApproxHash));
			ah->collid = PG_GET_COLLATION();
			fmgr_info_copy(&ah->fn, &tc->hash_extended_proc_finfo, fcinfo->flinfo->fn_mcxt);
			fcinfo->flinfo->fn_extra = ah;
		}
		sift_hll_add(h, DatumGetUInt64(FunctionCall2Coll(&ah->fn, ah->collid, PG_GETARG_DATUM(1), UInt64GetDatum(0))));
	}
	PG_RETURN_POINTER(h);
}

Datum
sift_approx_combine(PG_FUNCTION_ARGS)
{
	SiftHll    *a,
			   *b;

	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_POINTER(PG_GETARG_POINTER(0));
	}
	b = (SiftHll *) PG_GETARG_POINTER(1);
	if (PG_ARGISNULL(0))
	{
		MemoryContext aggcxt = approx_cxt(fcinfo);

		a = MemoryContextAlloc(aggcxt, sizeof(SiftHll));
		sift_hll_init(a, aggcxt);
	}
	else
		a = (SiftHll *) PG_GETARG_POINTER(0);
	sift_hll_merge(a, b);
	PG_RETURN_POINTER(a);
}

Datum
sift_approx_serial(PG_FUNCTION_ARGS)
{
	SiftHll    *h = (SiftHll *) PG_GETARG_POINTER(0);
	StringInfoData out;

	initStringInfo(&out);
	(void) sift_hll_encode(h, &out);
	PG_RETURN_BYTEA_P(cstring_to_text_with_len(out.data, out.len));
}

Datum
sift_approx_deserial(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	MemoryContext aggcxt = approx_cxt(fcinfo);
	SiftHll    *h;

	h = MemoryContextAlloc(aggcxt, sizeof(SiftHll));
	sift_hll_init(h, aggcxt);
	if (VARSIZE_ANY_EXHDR(b) > 0)
	{
		if (!sift_hll_check((const uint8 *) VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b)))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("malformed sift.approx_count_distinct state")));
		sift_hll_merge_blob(h, (const uint8 *) VARDATA_ANY(b));
	}
	PG_RETURN_POINTER(h);
}

Datum
sift_approx_final(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_INT64(0);
	PG_RETURN_INT64(sift_hll_estimate((SiftHll *) PG_GETARG_POINTER(0)));
}
