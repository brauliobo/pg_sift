/*
 * core/cachebudget_test.c - the registry of the backend caches (core/cachebudget.h): a backend that meets more kinds of cache than the tree had when the table was sized
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "fmgr.h"

#include "core/cachebudget.h"

PG_FUNCTION_INFO_V1(sift_test_cache_kinds_max);
PG_FUNCTION_INFO_V1(sift_test_cache_register_dummies);
PG_FUNCTION_INFO_V1(sift_test_cache_stress);

static uint64
dummy_oldest(void)
{
	return UINT64_MAX;
}

static bool
dummy_evict_one(void)
{
	return false;
}

static void
dummy_evict_all(void)
{
}

static const SiftCacheKind dummy_kind = {"test_dummy", dummy_oldest, dummy_evict_one, dummy_evict_all};

/* sift._test_cache_kinds_max() returns int4: the room of the table of kinds (SIFT_CACHE_MAX_KINDS) */
Datum
sift_test_cache_kinds_max(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(SIFT_CACHE_MAX_KINDS);
}

/* sift._test_cache_register_dummies(n int) returns int4: n kinds of cache that hold nothing registered with the budget, as the lazy first use of n more caches does; the registry is the backend's, so the test ends its session */
Datum
sift_test_cache_register_dummies(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(0);

	for (int i = 0; i < n; i++)
		sift_cache_register(&dummy_kind);
	PG_RETURN_INT32(n);
}

/* sift._test_cache_stress(on bool) returns bool: from now on every ask of a cache for room first drops every entry nobody pins; an entry a builder still uses after asking is then freed under it */
Datum
sift_test_cache_stress(PG_FUNCTION_ARGS)
{
	sift_cache_stress = PG_GETARG_BOOL(0);
	PG_RETURN_BOOL(sift_cache_stress);
}
#endif							/* SIFT_TESTING */
