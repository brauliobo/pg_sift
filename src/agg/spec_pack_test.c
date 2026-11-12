/*
 * agg/spec_pack_test.c - test-only SQL function sift._test_spec_pack_bits, compiled only with SIFT_TESTING and registered by the pg_sift_test extension:
 * a double of the plan spec goes the way it goes to a parallel worker (packed, written as text, read back, unpacked) and comes back as its bits.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "nodes/readfuncs.h"

#include "agg/spec.h"

PG_FUNCTION_INFO_V1(sift_test_spec_pack_bits);

Datum
sift_test_spec_pack_bits(PG_FUNCTION_ARGS)
{
	SiftAggSpec in = {0};
	SiftAggSpec *out;
	int64		bits = PG_GETARG_INT64(0);
	int64		back;

	memcpy(&in.est_groups, &bits, sizeof(bits));
	out = sift_aggspec_unpack((List *) stringToNode(nodeToString(sift_aggspec_pack(&in))));
	memcpy(&back, &out->est_groups, sizeof(back));
	PG_RETURN_INT64(back);
}

#endif
