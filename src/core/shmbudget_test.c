/*
 * core/shmbudget_test.c - the test functions of the ledger of the shared memory pools (core/shmbudget.h), compiled only with SIFT_TESTING.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#include "core/shmbudget.h"

PG_FUNCTION_INFO_V1(sift_test_shm_reserve);
PG_FUNCTION_INFO_V1(sift_test_shm_release);
PG_FUNCTION_INFO_V1(sift_test_shm_pool_used);
PG_FUNCTION_INFO_V1(sift_test_shm_total);

static SiftShmPoolId
pool_arg(FunctionCallInfo fcinfo)
{
	int32		pool = PG_GETARG_INT32(0);

	if (pool < 0 || pool >= SIFT_SHM_NPOOL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift test: the pool must be 0 to %d", SIFT_SHM_NPOOL - 1)));
	return (SiftShmPoolId) pool;
}

/* sift._test_shm_reserve(pool int4, bytes int8): takes bytes for a pool as a caller does; false when the ledger refuses */
Datum
sift_test_shm_reserve(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(sift_shm_reserve(pool_arg(fcinfo), (Size) PG_GETARG_INT64(1)));
}

/* sift._test_shm_release(pool int4, bytes int8): gives back bytes that sift._test_shm_reserve took */
Datum
sift_test_shm_release(PG_FUNCTION_ARGS)
{
	sift_shm_release(pool_arg(fcinfo), (Size) PG_GETARG_INT64(1));
	PG_RETURN_VOID();
}

/* sift._test_shm_pool_used(pool int4): what the ledger holds for the pool, the credits of the backends included */
Datum
sift_test_shm_pool_used(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_shm_pool_used(pool_arg(fcinfo)));
}

/* sift._test_shm_total(): what the ledger holds for all the pools */
Datum
sift_test_shm_total(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_shm_total_bytes());
}

#endif							/* SIFT_TESTING */
