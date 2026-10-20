/*
 * bm25/read_test.c - test-only SQL function sift._test_read_stats, compiled only with SIFT_TESTING and
 * registered by the pg_sift_test extension: what the read side of this backend allocated and did since
 * the last reset (term blob and cursor memory, passes of wide prefixes).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"
#include "utils/builtins.h"

#include "bm25/dict.h"
#include "bm25/posting.h"
#include "bm25/segment.h"
#include "bm25/prefix.h"

PG_FUNCTION_INFO_V1(sift_test_read_stats);

/* (reset bool) returns setof (key text, value int8) */
Datum
sift_test_read_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const struct
	{
		const char *key;
		uint64		value;
	}			stats[] = {
		{"dict_block_probes", sift_dict_probes},
		{"docdir_loads", sift_docdir_stats.loads},
		{"docdir_hits", sift_docdir_stats.hits},
		{"docdir_bytes_read", sift_docdir_stats.bytes_read},
		{"cursors", sift_post_stats.cursors},
		{"cursor_buffer_bytes", sift_post_stats.curbuf_bytes},
		{"blobs", sift_post_stats.blobs},
		{"blob_bytes", sift_post_stats.blob_bytes},
		{"prefix_passes", sift_prefix_stats.passes},
		{"prefix_windows", sift_prefix_stats.windows},
		{"prefix_direct", sift_prefix_stats.direct},
		{"prefix_peak_postings", sift_prefix_stats.peak},
	};

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < lengthof(stats); i++)
	{
		Datum		values[2] = {CStringGetTextDatum(stats[i].key), Int64GetDatum(stats[i].value)};
		bool		nulls[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
	}
	if (PG_GETARG_BOOL(0))
	{
		memset(&sift_prefix_stats, 0, sizeof(sift_prefix_stats));
		memset(&sift_post_stats, 0, sizeof(sift_post_stats));
		sift_dict_probes = 0;
		memset(&sift_docdir_stats, 0, sizeof(sift_docdir_stats));
	}
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
