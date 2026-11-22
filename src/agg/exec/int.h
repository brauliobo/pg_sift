/*
 * agg/exec/int.h - what the files of agg/exec/ share
 * The chunk of a top-N result and the functions the parts call in one another.
 */
#ifndef SIFT_AGG_EXEC_DIR_INT_H
#define SIFT_AGG_EXEC_DIR_INT_H

#include "agg/agg_int.h"

#ifdef SIFT_TESTING
extern bool sift_test_agg_wait;
extern int	sift_test_agg_pass_w;
extern void agg_test_wait(void);
#else
#define agg_test_wait() ((void) 0)
#endif
extern const CustomExecMethods agg_exec_methods;
extern void agg_shutdown(CustomScanState *node);
extern void agg_explain(CustomScanState *node, List *ancestors, ExplainState *es);
extern void run_counters(AggExec *c, SiftSrcCounters *cc);

extern TupleTableSlot * topn_next(AggExec *ex, TupleTableSlot *slot);

/*
 * The counters of a run of the Format 2.0 engine for EXPLAIN (ANALYZE): every worker counts in a struct of its own, with no atomic in a loop, and the leader merges
 * them once at the end of the scan; sift_agg_explain_v6 prints them as the compact lines of the plan.
 */
typedef struct SiftExplainStatsV6
{
	uint64		parts_scanned, parts_pruned_key, parts_pruned_time, l0_slabs_read;
	uint64		grans_scanned, grans_pruned_zone, grans_pruned_skip, grans_pruned_dict, grans_preagg;
	uint64		cfilt_grans_total, cfilt_grans_hit;
	uint64		cfilt_dict, cfilt_rle, cfilt_forbit, cfilt_delta, cfilt_fsst, cfilt_alp;
	uint64		late_mat_early_exits, late_mat_saved_bytes, sparse_decodes, adapt_reorders;
	uint64		aio_batches, aio_blocks;
	uint64		sip_probes, sip_passed, sip_grans_pruned, sip_bytes;
	uint64		bm25_hits, hybrid_col_grans;
	uint64		peak_mem_bytes, budget_mem_bytes, spill_count, spill_bytes;
	uint64		scope_l2_hits, scope_topk_ring_rows, scope_topk_spill_rows, heap_vet_rows;
	const char *group_mode;		/* "dict-direct", "salted-ht", "radix-ht", "stream-sorted", "preagg-only" */
	const char *par_strategy;	/* a parallel node: the strategy the planner chose; the counters are those of the participants once the node has shut down (par_ran) */
	uint64		par_morsels_stolen, par_radix_parts, par_partial_recs, par_partial_bytes;
	bool		par_ran, par_escalated;
	const char *fallback_reason;	/* NULL, or why a faster path was not taken */
} SiftExplainStatsV6;

extern void sift_agg_explain_v6(ExplainState *es, const SiftExplainStatsV6 *st, const char *simd, int workers_launched, int workers_planned);

#endif							/* SIFT_AGG_EXEC_DIR_INT_H */
