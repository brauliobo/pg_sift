/*
 * agg/test_gucs.c - the sift.test_ settings that outlive the legacy aggregate engine, defined only in the testing build
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/lock.h"
#include "core/visibility/visibility.h"
#include "utils/guc.h"

#include "agg/test_gucs.h"

int			sift_search_chunk_sbs = 32;
int			sift_agg_par_min_rows = 80000;

#ifdef SIFT_TESTING
static bool test_agg_wait;
static int	test_item_gate;

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

/* every participant of a parallel aggregate waits for the advisory lock of this number before each item, so that what runs meanwhile is after the open of all of them */
void
sift_v6_test_item_gate(void)
{
	sift_test_hold(test_item_gate);
}

void
sift_v6_define_test_gucs(void)
{
	DefineCustomIntVariable("sift.test_item_gate",
							"Testing: the participants of a parallel aggregate wait for the advisory lock of this number before each item (0 = none).",
							NULL, &test_item_gate, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_agg_par_min_rows",
							"Testing: rows below which an aggregate scan takes no workers (0 = the workers the settings allow, whatever they save).",
							NULL, &sift_agg_par_min_rows, 80000, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_wait",
							 "Testing: a top-N scan waits, once its source is open and before it reads a unit, for the advisory lock (20983, 1) to be free.",
							 NULL, &test_agg_wait, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_search_chunk_sbs",
							"Testing: superblocks of a chunk of a search scan.",
							NULL, &sift_search_chunk_sbs, 32, 1, 1024, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#endif
