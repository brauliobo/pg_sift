/*
 * bm25/read_test.c - test-only SQL function sift._test_read_stats, compiled only with SIFT_TESTING and
 * registered by the pg_sift_test extension: what the read side of this backend allocated and did since
 * the last reset (term blob and cursor memory, passes of wide prefixes).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"
#include "utils/builtins.h"

#include "access/genam.h"
#include "col/wbuf.h"
#include "bm25/dict.h"
#include "bm25/maint.h"
#include "bm25/wbuf.h"
#include "bm25/posting.h"
#include "bm25/segment.h"
#include "bm25/prefix.h"

PG_FUNCTION_INFO_V1(sift_test_read_stats);
PG_FUNCTION_INFO_V1(sift_test_wbuf_reads);

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
		{"dict_scan_blocks", sift_dict_scan_blocks},
		{"docdir_loads", sift_docdir_stats.loads},
		{"docdir_hits", sift_docdir_stats.hits},
		{"docdir_bytes_read", sift_docdir_stats.bytes_read},
		{"cursors", sift_post_stats.cursors},
		{"cursor_buffer_bytes", sift_post_stats.curbuf_bytes},
		{"blobs", sift_post_stats.blobs},
		{"blob_bytes", sift_post_stats.blob_bytes},
		{"blob_hits", sift_post_stats.blob_hits},
		{"segcache_bytes", sift_segcache_bytes()},
		{"segcache_evicted", sift_segcache_stats.evicted},
		{"segcache_refused", sift_segcache_stats.refused},
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
		sift_dict_scan_blocks = 0;
		memset(&sift_docdir_stats, 0, sizeof(sift_docdir_stats));
		memset(&sift_segcache_stats, 0, sizeof(sift_segcache_stats));
	}
	return (Datum) 0;
}

/* (index regclass) -> (walked, ndocs, due): the documents queries walked in the open write buffer of a sift index, the documents it holds and whether their reads have paid for its promotion */
Datum
sift_test_wbuf_reads(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	tupdesc;
	Datum		values[3] = {0, 0, 0};
	bool		nulls[3] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");	/* check-errors: allow */
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_OPEN)
		{
			uint64		ndocs,
						sum_dl,
						npages;

			sift_wbuf_summary(index, &snap->segs[i], &ndocs, &sum_dl, &npages);
			values[0] = Int64GetDatum(sift_cwbuf_decoded(index, snap->segs[i].gen));
			values[1] = Int64GetDatum(ndocs);
		}
	values[2] = BoolGetDatum(sift_maint_wbuf_read_due(index));
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
