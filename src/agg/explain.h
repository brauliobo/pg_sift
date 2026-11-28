/*
 * agg/explain.h - the EXPLAIN lines of SiftAggV6 that name what the scan reads (v6_explain.c)
 */
#ifndef SIFT_AGG_V6_EXPLAIN_H
#define SIFT_AGG_V6_EXPLAIN_H

#include "postgres.h"

#include "commands/explain.h"
#include "utils/relcache.h"

#include "agg/engine.h"
#include "agg/ext.h"

/* Filters (the quals of the scan: the index's and the batches') and Search (the text query of a sift index) of the scan of heap, read through index (NULL for a sift_table) */
extern void sift_v6_explain_reads(const SiftV6Spec *spec, Relation heap, Relation index, ExplainState *es);
/* Group Keys: the columns the groups are of, when the statement names them as they are, and the keys the scan computes from a column as `column: expression`, with the calendar step a key keeps its value over */
extern void sift_v6_explain_group_keys(CustomScanState *css, List *ancestors, const SiftV6Spec *spec, ExplainState *es);
/* EXPLAIN ANALYZE: the grouping sets summed from the one the scan updates (agg/group/sets.c) */
extern void sift_v6_explain_sets(const SiftV6Spec *spec, ExplainState *es);
/* the same for the keys of the stream aggregate (ints: the oid of its source, the range table index of the relation and whether the source is a sift_table): columns, and the bins it computes */
extern void sift_v6_explain_stream_keys(const List *ints, const List *keys, ExplainState *es);
/* Pushed Aggregates: the aggregates the scan runs (a CASE of an aggregate as its FILTER), as the statement names their columns */
extern void sift_v6_explain_pushed_aggs(CustomScanState *css, List *ancestors, ExplainState *es);
/* Lookup Joins: the relations the plans of the dimensions of a star join read; rows_shared: the rows of their tables the leader of a parallel scan shared with its workers */
extern void sift_v6_explain_lookup_joins(CustomScanState *css, const SiftV6Ext *ext, uint64 rows_shared, ExplainState *es);

/* an expression of a statement being planned as the statement names its relations: the columns of every relation of its range table (prefix: qualified by their relation) */
extern char *sift_v6_deparse_planning(PlannerInfo *root, Node *e, bool prefix);

/* Top-N: the sort keys and the limit of the groups (the spec of the aggregate scan, when it gives the best groups in order) */
extern void sift_v6_explain_topn_groups(const SiftV6Spec *spec, ExplainState *es);

/*
 * Unnest, Map Keys and Unnest Value: the lateral columnar unnest of the scan, the members of documents it reads by a path (SiftV6Spec.jmembers) as
 * `attname['key'] type, ..`, and the array whose values it reads (desc: the attributes of the host)
 */
extern void sift_v6_explain_doc_reads(const SiftV6Spec *spec, TupleDesc desc, ExplainState *es);

/*
 * The counters of a run of the Format 2.0 engine for EXPLAIN (ANALYZE): every worker counts in a struct of its own, with no atomic in a loop, and the leader merges
 * them once at the end of the scan; sift_agg_explain_v6 prints them as the compact lines of the plan.
 */
typedef struct SiftExplainStatsV6
{
	uint64		parts_scanned, parts_pruned_key, parts_pruned_time, l0_slabs_read;
	uint64		grans_scanned, grans_pruned_zone, grans_pruned_skip, grans_pruned_dict, grans_preagg, grans_preagg_delta, grans_preagg_super, grans_preagg_net, grans_except_read, src_filters, point_rows;
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
	int64		dict_shared_hits, dict_shared_misses;	/* VERBOSE: the segment dictionaries the backend copied from shared memory, and parsed itself, while the node ran (agg/stats.h) */
} SiftExplainStatsV6;

extern void sift_agg_explain_v6(ExplainState *es, const SiftExplainStatsV6 *st, const char *simd, int workers_launched, int workers_planned);

#endif							/* SIFT_AGG_V6_EXPLAIN_H */
