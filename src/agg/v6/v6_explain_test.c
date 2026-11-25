/*
 * agg/v6/v6_explain_test.c - test-only SQL function of the Format 2.0 EXPLAIN lines (agg/v6/v6_explain.c), compiled only with SIFT_TESTING and registered by
 * the pg_sift_test extension: sift._test_explain_v6(scenario, format) prints the lines of a run with the counters of one of four scenarios: 1 a scan of a
 * Format 2.0 index, 2 a scope top-N, 3 a hybrid BM25 and column scan, 4 a runtime hash-join filter with a spill and a fallback.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "optimizer/cost.h"
#include "utils/builtins.h"

#include "core/explain.h"

#include "agg/v6/v6_explain.h"

PG_FUNCTION_INFO_V1(sift_test_explain_v6);

Datum
sift_test_explain_v6(PG_FUNCTION_ARGS)
{
	const int	scenario = PG_GETARG_INT32(0);
	char	   *fmt = text_to_cstring(PG_GETARG_TEXT_PP(1));
	const bool	verbose = PG_NARGS() > 2 && PG_GETARG_BOOL(2);
	SiftExplainStatsV6 st = {0};
	ExplainState *es = NewExplainState();
	int			workers = 4;

	es->analyze = true;
	es->verbose = verbose;
	es->format = strcmp(fmt, "json") == 0 ? EXPLAIN_FORMAT_JSON : EXPLAIN_FORMAT_TEXT;
	switch (scenario)
	{
		case 1:
			st = (SiftExplainStatsV6) {.parts_scanned = 4, .parts_pruned_key = 188, .parts_pruned_time = 8, .l0_slabs_read = 1,
									   .grans_scanned = 62, .grans_pruned_zone = 410, .grans_pruned_skip = 118, .grans_pruned_dict = 12, .grans_preagg = 48,
									   .cfilt_grans_total = 62, .cfilt_grans_hit = 57, .cfilt_dict = 38, .cfilt_rle = 14, .cfilt_forbit = 8, .cfilt_fsst = 2,
									   .late_mat_saved_bytes = 19293798, .aio_batches = 14, .aio_blocks = 112,
									   .peak_mem_bytes = 2936012, .budget_mem_bytes = 4194304, .group_mode = "dict-direct"};
			break;
		case 2:
			workers = 0;
			st = (SiftExplainStatsV6) {.scope_l2_hits = 1, .scope_topk_ring_rows = 20, .aio_batches = 1, .aio_blocks = 3,
									   .peak_mem_bytes = 131072, .budget_mem_bytes = 4194304};
			break;
		case 3:
			workers = 0;
			st = (SiftExplainStatsV6) {.bm25_hits = 312, .hybrid_col_grans = 4, .cfilt_grans_total = 4, .cfilt_grans_hit = 4, .cfilt_forbit = 4,
									   .late_mat_saved_bytes = 1258291, .aio_batches = 2, .aio_blocks = 9,
									   .peak_mem_bytes = 421888, .budget_mem_bytes = 4194304};
			break;
		default:
			st = (SiftExplainStatsV6) {.parts_scanned = 6, .parts_pruned_time = 194,
									   .grans_scanned = 112, .grans_pruned_zone = 68, .grans_pruned_skip = 84,
									   .cfilt_grans_total = 112, .cfilt_grans_hit = 99, .cfilt_rle = 44, .cfilt_forbit = 68,
									   .sip_probes = 800000, .sip_passed = 11200, .sip_grans_pruned = 84, .sip_bytes = 65536,
									   .aio_batches = 22, .aio_blocks = 240, .peak_mem_bytes = 4089446, .budget_mem_bytes = 4194304,
									   .spill_count = 1, .spill_bytes = 1258291, .group_mode = "radix-ht",
									   .fallback_reason = "preagg_skipped (runtime SIP filter on non-cube column 'entity_id')"};
	}
	es->indent = 1;
	ExplainBeginOutput(es);
	sift_agg_explain_v6(es, &st, "avx512", workers, workers);
	ExplainEndOutput(es);
	PG_RETURN_TEXT_P(cstring_to_text(es->str->data));
}

#endif
