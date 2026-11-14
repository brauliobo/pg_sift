/*
 * core/check.c - sift.check(): the steps that report instead of raise, and the walker that picks the verifier of an index or table
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"

#include "col/pg.h"
#include "core/check.h"

/* The errors that are the index's state and not the call's: a corruption, or options and a layout that are not those it was built with */
static bool
is_problem(int sqlerrcode)
{
	return sqlerrcode == ERRCODE_INDEX_CORRUPTED || sqlerrcode == ERRCODE_DATA_CORRUPTED || sqlerrcode == ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE;
}

static void
put_problem(SiftCheck *ck, uint64 segment, const ErrorData *e)
{
	Datum		v[4] = {ObjectIdGetDatum(ck->relid), Int64GetDatum((int64) segment), CStringGetTextDatum(e->message), 0};
	bool		nulls[4] = {false, segment == SIFT_CHECK_WHOLE, false, e->detail == NULL};

	v[3] = e->detail ? CStringGetTextDatum(e->detail) : (Datum) 0;
	tuplestore_putvalues(ck->rows, ck->desc, v, nulls);
	ck->nproblems++;
}

bool
sift_check_step(SiftCheck *ck, uint64 segment, SiftCheckStep step, void *arg)
{
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	bool		sound = true;

	if (ck == NULL)
	{
		step(arg);
		return true;
	}
	if (sift_check_full(ck))
		return false;
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(old);
	PG_TRY();
	{
		step(arg);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(old);
		e = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old);
		CurrentResourceOwner = owner;
		if (!is_problem(e->sqlerrcode))
			ReThrowError(e);
		put_problem(ck, segment, e);
		FreeErrorData(e);
		sound = false;
	}
	PG_END_TRY();
	return sound;
}

SiftCheck *
sift_check_begin(FunctionCallInfo fcinfo, Oid relid, int max_problems)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftCheck  *ck = palloc0(sizeof(SiftCheck));

	if (max_problems < 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("max_problems must be at least 1")));
	InitMaterializedSRF(fcinfo, 0);
	ck->relid = relid;
	ck->rows = rsi->setResult;
	ck->desc = rsi->setDesc;
	ck->max_problems = max_problems;
	return ck;
}

void
sift_check_relation(SiftCheck *ck, Oid relid, bool heapallindexed)
{
	if (get_rel_relkind(relid) != RELKIND_INDEX)
		sift_tbl_check(ck, relid);
	else if (sift_col_is_index_oid(relid))
		sift_col_check(ck, relid, heapallindexed);
	else
		sift_bm25_check(ck, relid, heapallindexed);
}

PG_FUNCTION_INFO_V1(sift_check_one);

/* sift._check(object, heapallindexed, max_problems): the problems of one index or table, a row each */
Datum
sift_check_one(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	sift_check_relation(sift_check_begin(fcinfo, relid, PG_GETARG_INT32(2)), relid, PG_GETARG_BOOL(1));
	return (Datum) 0;
}
