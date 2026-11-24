/*
 * agg/v6/v6_test_gucs.c - the sift.test_ settings that outlive the legacy aggregate engine, defined only in the testing build
 */
#include "postgres.h"

#include "utils/guc.h"

#include "agg/v6/v6_test_gucs.h"

int			sift_search_chunk_sbs = 32;

#ifdef SIFT_TESTING
void
sift_v6_define_test_gucs(void)
{
	DefineCustomIntVariable("sift.test_search_chunk_sbs",
							"Testing: superblocks of a chunk of a search scan.",
							NULL, &sift_search_chunk_sbs, 32, 1, 1024, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#endif
