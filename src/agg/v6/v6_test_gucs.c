/*
 * agg/v6/v6_test_gucs.c - the sift.test_ settings that outlive the legacy aggregate engine, defined only in the testing build
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/lock.h"
#include "utils/guc.h"

#include "agg/v6/v6_test_gucs.h"

int			sift_search_chunk_sbs = 32;

#ifdef SIFT_TESTING
static bool test_agg_wait;

void
sift_v6_test_wait(void)
{
	LOCKTAG		tag;

	if (!test_agg_wait)
		return;
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, 1, 2);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

void
sift_v6_define_test_gucs(void)
{
	DefineCustomBoolVariable("sift.test_agg_wait",
							 "Testing: a top-N scan waits, once its source is open and before it reads a unit, for the advisory lock (20983, 1) to be free.",
							 NULL, &test_agg_wait, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_search_chunk_sbs",
							"Testing: superblocks of a chunk of a search scan.",
							NULL, &sift_search_chunk_sbs, 32, 1, 1024, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#endif
