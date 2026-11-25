/*
 * table/f2/f2_chain_cache_test.c - sift._test_chain_cache_pages(): the pages of column delta chains the cache of f2_chain_cache.h has read, for the tests that bound them.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#include "table/f2/f2_chain_cache.h"

PG_FUNCTION_INFO_V1(sift_test_chain_cache_pages);

Datum
sift_test_chain_cache_pages(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_chain_cache_pages);
}

#endif
