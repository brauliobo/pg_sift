\echo Use "ALTER EXTENSION pg_sift_test UPDATE TO '1.1'" to execute this file. \quit

-- Format major 2 (src/col/f2/): a sift_store_test index turned into an index of the new format, and the layers on top of it
CREATE FUNCTION sift._test_f2_scan_counters(reset bool) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_scan_counters' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_prune_counters(reset bool) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_prune_counters' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_plan_cache(reset bool) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_plan_cache' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_plan_cache_shm(reset bool) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_plan_cache_shm' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_plan_cache_drop() RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_plan_cache_drop' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_plan_snapshot(index regclass, OUT partitions int4, OUT parts int8, OUT total_rows int8, OUT manifest_version int8) RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_plan_snapshot' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_init(index regclass) RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_init' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_meta(index regclass, OUT major int4, OUT minor int4, OUT struct_size int4, OUT high_water int8, OUT meta_slab_end int8,
								   OUT npartitions int4, OUT manifest_version int8, OUT next_gen int8, OUT next_part_id int8, OUT total_rows int8,
								   OUT total_dead int8, OUT partdir_level int4)
	RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_meta' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_meta_resize(rel regclass, size int4) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_f2_meta_resize' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_scopes(index regclass, first_ord int4, n int4) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_scopes' LANGUAGE C STRICT;
-- the summaries of the scopes of an index of format major 2 read as a query will: the cube of every key (month NULL: all time), the newest rows of a key, the state of the object
CREATE FUNCTION sift._test_f2_scope_keys(index regclass, scope_ord int4, month int4 DEFAULT NULL, OUT scope_key int8, OUT rows int8, OUT time_min int8, OUT time_max int8, OUT sums float8[])
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_scope_keys' LANGUAGE C;
CREATE FUNCTION sift._test_f2_scope_latest(index regclass, scope_ord int4, scope_key int8, n int4) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_scope_latest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_scope_info(index regclass, scope_ord int4) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_scope_info' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_scope_current(index regclass, scope_ord int4) RETURNS bool AS 'MODULE_PATHNAME', 'sift_test_f2_scope_current' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_scope_refresh(index regclass) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_f2_scope_refresh' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_scope_dir(index regclass)
	RETURNS TABLE(scope_ord int4, flags int4, header_blk int8, meta_gen int8)
	AS 'MODULE_PATHNAME', 'sift_test_f2_scope_dir' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_sections(index regclass, required_unknown bool, inline_cap int4, max_extent int4, nbytes int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_sections' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_add_part(index regclass, part_id int8, plo int8, nrows int4, base_key int8, tid_base int8, xmin int8 DEFAULT 0,
									   level int4 DEFAULT 0, mode int4 DEFAULT 0, preagg bool DEFAULT false, reverse bool DEFAULT false) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_f2_add_part' LANGUAGE C;
CREATE FUNCTION sift._test_f2_drop_part(index regclass, part_id int8, plo int8, gen int8) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_drop_part' LANGUAGE C;
CREATE FUNCTION sift._test_f2_split(index regclass, part_id int8, plo int8, split int8) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_split' LANGUAGE C;
CREATE FUNCTION sift._test_f2_split_many(index regclass, part_id int8, plo int8, n int4, step int8) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_f2_split_many' LANGUAGE C;
CREATE FUNCTION sift._test_f2_merge(index regclass, keep_id int8, keep_lo int8, drop_id int8, drop_lo int8) RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_merge' LANGUAGE C;
CREATE FUNCTION sift._test_f2_partitions(index regclass, OUT part_id int8, OUT key_lo int8, OUT key_hi int8, OUT nparts int4, OUT total_rows int8,
										 OUT total_dead int8, OUT l0_parts int4, OUT has_slab bool, OUT has_pending bool)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_partitions' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_parts(index regclass, part_id int8, plo int8, OUT part_gen int8, OUT level int4, OUT flags int4, OUT nrows int8,
									OUT ndead int8, OUT has_dv bool, OUT key_lo int8, OUT key_hi int8, OUT has_xid bool)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_parts' LANGUAGE C;
CREATE FUNCTION sift._test_f2_range(index regclass, lo int8, hi int8, OUT partitions int4, OUT pages int4, OUT first_part int8) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_range' LANGUAGE C;
CREATE FUNCTION sift._test_f2_check(index regclass) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_check' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_check_keys(index regclass) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_check_keys' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_leaves(index regclass)
	RETURNS TABLE(tid tid, leaf text, elem int4, val text, tag int4, payload text)
	AS 'MODULE_PATHNAME', 'sift_test_f2_leaves' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_array_layout(index regclass) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_array_layout' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_preagg_index(index regclass, dim int4, measure int4)
	RETURNS TABLE(parts int4, granules int4, with_dead int4, corrected int4, rescanned int4, mismatched int4, dead int8)
	AS 'MODULE_PATHNAME', 'sift_test_f2_preagg_index' LANGUAGE C STRICT;
-- The cube of every granule less the union of its dead rows and the rows a column has a value on, against the cube of the rows that are left; the union and the two lists one after the other
CREATE FUNCTION sift._test_f2_preagg_excl(index regclass, dim_col int4, measure_col int4, null_col int4, OUT granules int4, OUT with_excl int4, OUT corrected int4, OUT rescanned int4, OUT mismatched int4, OUT naive_mismatched int4, OUT dead int8, OUT not_null int8, OUT overlap int8) RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_preagg_excl' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_alloc(index regclass, nblocks int4, OUT start int8, OUT nblocks_out int4) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_alloc' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_alloc_stream(index regclass, nblocks int8) RETURNS TABLE(start int8, nblocks_out int4) AS 'MODULE_PATHNAME', 'sift_test_f2_alloc_stream' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_alloc_below(index regclass, nblocks int8, max_blk int8) RETURNS TABLE(start int8, nblocks_out int4) AS 'MODULE_PATHNAME', 'sift_test_f2_alloc_below' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_meta_alloc(index regclass) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_meta_alloc' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_release(index regclass, start int8, nblocks int4, safe text) RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_release' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_reclaim(index regclass) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_reclaim' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_extend_file(index regclass, blocks int8) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_extend_file' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_tail_shrink(index regclass, OUT blocks_truncated int8, OUT lock_skipped bool, OUT high_water int8, OUT file_blocks int8) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_tail_shrink' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_meta_inline(index regclass, OUT inline_off int4, OUT inline_len int4, OUT inline_parts int4, OUT flagged bool) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_meta_inline' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_fsm(index regclass) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_fsm' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_dump(index regclass, part_id int8, plo int8, gen int8, base_key int8, tid_base int8, mode int4 DEFAULT 0) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_dump' LANGUAGE C;
CREATE FUNCTION sift._test_f2_stage(index regclass, nrows int4, base_key int8, tid_base int8, xmin int8 DEFAULT 0, step int8 DEFAULT 1) RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_stage' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_stage_flush(index regclass) RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_stage_flush' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_stage_rows(index regclass) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_stage_rows' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_slab(index regclass, part_id int8, plo int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_slab' LANGUAGE C;
CREATE FUNCTION sift._test_f2_slab_flush(index regclass, part_id int8, plo int8) RETURNS bool AS 'MODULE_PATHNAME', 'sift_test_f2_slab_flush' LANGUAGE C;
CREATE FUNCTION sift._test_f2_delete(index regclass, part_id int8, plo int8, tid_base int8, first int4, count int4, step int4 DEFAULT 1, xmax int8 DEFAULT 0) RETURNS void AS 'MODULE_PATHNAME', 'sift_test_f2_delete' LANGUAGE C;
CREATE FUNCTION sift._test_f2_pending(index regclass, part_id int8, plo int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_pending' LANGUAGE C;
CREATE FUNCTION sift._test_f2_dv(index regclass, part_id int8, plo int8, gen int8, OUT granule int4, OUT kind text, OUT ndead int4, OUT unsettled bool) RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_dv' LANGUAGE C;
CREATE FUNCTION sift._test_f2_visible(index regclass, part_id int8, plo int8, gen int8, OUT visible int8, OUT judged text) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_visible' LANGUAGE C;
CREATE FUNCTION sift._test_f2_seal(index regclass, part_id int8, plo int8, gen int8, OUT sealed bool, OUT flags int4) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_seal' LANGUAGE C;
CREATE FUNCTION sift._test_f2_part_xids(index regclass, part_id int8, plo int8, gen int8, OUT min_xid int8, OUT max_xid int8) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_part_xids' LANGUAGE C;
CREATE FUNCTION sift._test_f2_vers(index regclass, part_id int8, plo int8, gen int8, OUT granules int4, OUT single_xact int4, OUT vers_bytes int8) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_vers' LANGUAGE C;
CREATE FUNCTION sift._test_f2_vis_count(index regclass, heap regclass, stale bool DEFAULT false, OUT visible int8, OUT heap_pages int8) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_vis_count' LANGUAGE C;
CREATE FUNCTION sift._test_f2_preagg(index regclass, part_id int8, plo int8, gen int8, granule int4) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_preagg' LANGUAGE C;
CREATE FUNCTION sift._test_f2_candidates(index regclass, part_id int8, plo int8) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_candidates' LANGUAGE C;
CREATE FUNCTION sift._test_f2_compact(index regclass, part_id int8, plo int8, gens int8[], preagg bool DEFAULT false, remap bool DEFAULT true, OUT done bool, OUT new_gen int8,
									  OUT rows_in int8, OUT rows_out int8, OUT dead_dropped int8, OUT aborted_dropped int8, OUT frozen int8, OUT granules_copied int8, OUT granules_written int8, OUT bytes_out int8,
									  OUT dict_cols_remapped int8, OUT dict_codes_remapped int8, OUT writer_bytes int8)
	RETURNS record AS 'MODULE_PATHNAME', 'sift_test_f2_compact' LANGUAGE C;
CREATE FUNCTION sift._test_f2_tstride(index regclass, nrows int4) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_tstride' LANGUAGE C;
CREATE FUNCTION sift._test_f2_encs(index regclass, part_id int8, plo int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_encs' LANGUAGE C;
CREATE FUNCTION sift._test_f2_leaf_zones(index regclass, part_id int8, plo int8, leaf int4, op int4, literal text) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_leaf_zones' LANGUAGE C;
CREATE FUNCTION sift._test_f2_part_bytes(index regclass, part_id int8, plo int8) RETURNS int8[] AS 'MODULE_PATHNAME', 'sift_test_f2_part_bytes' LANGUAGE C;
CREATE FUNCTION sift._test_f2_part_digest(index regclass, part_id int8, plo int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_part_digest' LANGUAGE C;
CREATE FUNCTION sift._test_f2_order(index regclass, part_id int8, plo int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_order' LANGUAGE C;
CREATE FUNCTION sift._test_f2_plan(index regclass, part_id int8, plo int8) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_plan' LANGUAGE C;
CREATE FUNCTION sift._test_f2_qset(index regclass, op text) RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_f2_qset' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_enc(kind text, pattern text, nrows int4, nulls bool DEFAULT false, tags bool DEFAULT false, wrap text DEFAULT 'none', tier text DEFAULT 'cold') RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_enc' LANGUAGE C STRICT;
-- A column with a value on nvalid of nrows rows as one granule: the validity the writer chose, the rows read without the values, the values and the vector read back
CREATE FUNCTION sift._test_f2_sparse(nrows int4, nvalid int4) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_sparse' LANGUAGE C STRICT;
-- The leaf of a key of jsonb documents of the shapes the arguments count: how the granule keeps which rows have the key, and the rows read back by sift_granule_decode_key_present
CREATE FUNCTION sift._test_f2_key(nrows int4, nvalid int4, njnull int4 DEFAULT 0, noverflow int4 DEFAULT 0, nrootkey int4 DEFAULT 0, presence_only bool DEFAULT false) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_key' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_fsst(pattern text, nstrings int4, detail bool DEFAULT false) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_f2_fsst' LANGUAGE C STRICT;
-- The format major of an index of either format, from its block 0 (core/meta_peek.c): 1 or 2, 0 while it has no block
CREATE FUNCTION sift._test_index_format(index regclass) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_index_format' LANGUAGE C STRICT;
-- The Top-N tournament (src/agg/topn_v6_test.c): the rows it outputs over arrays cut into granules, parts and partitions, and the units it did not read.
CREATE FUNCTION sift._test_topn(k0 int8[], k1 int8[], k2 int8[], keep bool[], descs bool[], nulls_firsts bool[], granule_rows int4, granules_per_part int4,
								parts_per_partition int4, slab_rows int4, lim int8, off int8, with_ties bool DEFAULT false, workers int4 DEFAULT 1,
								frontier bool DEFAULT false) RETURNS TABLE(rank int4, idx int4)
	AS 'MODULE_PATHNAME', 'sift_test_topn' LANGUAGE C;
CREATE FUNCTION sift._test_topn_stats(OUT partitions_pruned int8, OUT parts_pruned int8, OUT granules_pruned int8, OUT granules_scanned int8,
									  OUT early_terminated bool)
	AS 'MODULE_PATHNAME', 'sift_test_topn_stats' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f64_order(x float8) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_f64_order' LANGUAGE C STRICT;
-- The cardinality estimator (src/agg/plan/card_est_test.c): the estimate of predicates over the statistics folded from arrays.
CREATE FUNCTION sift._test_card_est(c0 int8[], c1 int8[], c2 int8[], doc bool[], present bool[], preds text, granule_rows int4 DEFAULT 1024,
									granules_per_partition int4 DEFAULT 16, sample int4 DEFAULT 64)
	RETURNS TABLE(est_rows float8, selectivity float8, g_eff float8, g_total float8, confidence float8, exact_zero bool, used_scope bool, ndv_c1 float8)
	AS 'MODULE_PATHNAME', 'sift_test_card_est' LANGUAGE C;
-- The first granule of the tail tie run of a window partition (src/agg/window_test.c)
CREATE FUNCTION sift._test_window_tail_tie(vals int8[], granule int4) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_window_tail_tie' LANGUAGE C STRICT;

-- 2048-row operators (src/agg/kern6_test.c): every SIMD level against scalar, through sift.simd.
CREATE FUNCTION sift._test_aggkern6_equiv(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_aggkern6_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_aggkern6_level() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_aggkern6_level' LANGUAGE C STRICT;
-- Salted hash table, array element masks and the cost of a SiftAgg path (src/agg/v6_test.c): the number of failed cases.
CREATE FUNCTION sift._test_salted_ht(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_salted_ht' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_array_elem_mask(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_array_elem_mask' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_agg_cost(g_eff float8, b_dec float8, n_groups float8, f_join float8, selectivity float8, is_preagg bool,
									is_streaming bool, max_workers int4, OUT workers int4, OUT startup float8, OUT total float8)
	AS 'MODULE_PATHNAME', 'sift_test_agg_cost' LANGUAGE C STRICT;
-- Group table beyond 16 byte keys (src/agg/ht_test.c): the number of failed cases.
CREATE FUNCTION sift._test_ht_grow() RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_ht_grow' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_ht_ext(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_ht_ext' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_dist_set(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_dist_set' LANGUAGE C STRICT;
-- The groups of strings in the table of strings of a collation (src/agg/ht_test.c); 0A000 for a collation that is not deterministic
CREATE FUNCTION sift._test_ht_text_groups(vals text[], collation oid) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_ht_text_groups' LANGUAGE C STRICT;
-- Two-phase radix-partitioned parallel aggregation (src/agg/par/radix_test.c): workers launched, groups out, wrong groups, partitions spilled, chunks taken, microseconds from the launch to the last output and to the end of the scan.
CREATE FUNCTION sift._test_radix_par(seed int8, workers int4, granules int4, groups int4, mode int4, local_cap int4, max_chunks int4,
									 OUT launched int4, OUT out_groups int8, OUT bad int8, OUT spilled int4, OUT chunks int4, OUT par_us int8, OUT scan_us int8)
	AS 'MODULE_PATHNAME', 'sift_test_radix_par' LANGUAGE C STRICT;
-- Guided work queue of the parallel scans (src/agg/par/morsel_test.c): workers launched, claims, granules, granules not visited exactly once, claims out of protocol, longest claim, length of the last claim.
CREATE FUNCTION sift._test_morsel_par(seed int8, workers int4, parts int4, granules int4,
									  OUT launched int4, OUT claims int8, OUT total int8, OUT wrong int8, OUT bad_claims int8, OUT max_run int4, OUT tail_run int4)
	AS 'MODULE_PATHNAME', 'sift_test_morsel_par' LANGUAGE C STRICT;
-- Shared dimension table (src/agg/par/dim_shm_test.c): workers launched, buckets, probes answered, wrong answers and keys, fits the budget, bytes, microseconds of the build.
CREATE FUNCTION sift._test_dim_shm(seed int8, workers int4, nrows int4, domain int4, probes int4,
								   OUT launched int4, OUT buckets int4, OUT answered int8, OUT wrong int8, OUT fits bool, OUT bytes int8, OUT build_us int8)
	AS 'MODULE_PATHNAME', 'sift_test_dim_shm' LANGUAGE C STRICT;
-- Adaptive execution (src/agg/adaptive_test.c): the order of the conjuncts that the measures give, the circuit breaker of the join filter, the tiers of the groups and
-- the grouping sets under a budget, each against the same work with no limit.
CREATE FUNCTION sift._test_apipe(OUT moved_best_first bool, OUT barrier_violations int4, OUT reorders int4, OUT near_ranks_stable bool, OUT drift_followed bool,
								 OUT explored bool, OUT dense_after_empty bool)
	AS 'MODULE_PATHNAME', 'sift_test_apipe' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_apipe_filter(a int8[], b int8[], barrier bool, OUT matched int8, OUT reorders int4, OUT division_first bool)
	AS 'MODULE_PATHNAME', 'sift_test_apipe_filter' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_sip_adapt(granules int4, head_granules int4, head_match int4, tail_match int4, OUT rows_probed int8, OUT rows_total int8,
									 OUT auto_disables int4, OUT auto_reenables int4, OUT disabled_at_end bool, OUT reject_after_reenable_pct int4,
									 OUT level2_prunes bool, OUT missed int8)
	AS 'MODULE_PATHNAME', 'sift_test_sip_adapt' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_group_tiers(n int8, domain int8, base int8, start int4, budget int8, hot int4, skew int4, OUT groups int8, OUT rows int8,
									   OUT key_sum int8, OUT sum_sum int8, OUT mix int8, OUT tier int4, OUT visited int4, OUT spilled_bytes int8, OUT depth int4,
									   OUT same bool)
	AS 'MODULE_PATHNAME', 'sift_test_group_tiers' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_rollup_spill(masks int8[], n int8, groups int8, max_bytes int8, skew bool, dist bool, OUT nodes_spilled int4, OUT bytes_spilled int8,
										OUT depth int4, OUT groups_out int8, OUT same bool)
	AS 'MODULE_PATHNAME', 'sift_test_rollup_spill' LANGUAGE C STRICT;
-- The dimensions of a star join in one block (src/agg/par/star_shm_test.c): the rows of a fact table joined to categories, items (which point to the categories: a snowflake) and
-- regions through the filters and tables of the bundle that the leader and the workers built, and what the scan did.
CREATE FUNCTION sift._test_star(f_item int8[], f_region int8[], i_id int8[], i_cat int8[], i_pay int8[], c_id int8[], c_pay int8[], r_id int8[], r_pay int8[],
								kind_cat int4, kind_item int4, kind_region int4, cats_unique bool, items_unique bool, with_regions bool, workers int4,
								granule int4 DEFAULT 1024)
	RETURNS TABLE(ord int4, item_pay int8, cat_pay int8, region_pay int8)
	AS 'MODULE_PATHNAME', 'sift_test_star' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_star_stats(OUT items_raw int4, OUT items_surviving int4, OUT cats_surviving int4, OUT any_empty bool, OUT fact_rows_read int8,
									  OUT granules_total int4, OUT granules_pruned int4, OUT rows_probed int8, OUT auto_disables int4, OUT elided int4,
									  OUT launched int4, OUT bundle_bytes int8)
	AS 'MODULE_PATHNAME', 'sift_test_star_stats' LANGUAGE C STRICT;
-- Exception streams of JSONB paths and sorted streaming GROUP BY / Top-N (src/agg/stream_test.c): the number of failed cases.
CREATE FUNCTION sift._test_variant_eval(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_variant_eval' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_stream_exec(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_stream_exec' LANGUAGE C STRICT;

-- Runtime join filter (src/agg/sip_test.c): the number of failed cases, and the filter as the semi-filter of a join over two arrays.
CREATE FUNCTION sift._test_sip(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_sip' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_sip_filter(fact int8[], dim int8[], as_int4 bool) RETURNS TABLE(ord int4, pass bool)
	AS 'MODULE_PATHNAME', 'sift_test_sip_filter' LANGUAGE C STRICT;
-- Streaming window functions and DISTINCT ON over sorted arrays (src/agg/window_test.c): the results of each row as text, and the counters of the last run.
CREATE FUNCTION sift._test_window(part int8[], ord int8[], arg int8[], argf float8[], funcs text[], batch_rows int4 DEFAULT 2048, mode text DEFAULT 'window',
								  topk int8 DEFAULT 0, off int8 DEFAULT 0, lim int8 DEFAULT NULL, act bool[] DEFAULT NULL, keys_i32 bool DEFAULT false,
								  granule int4 DEFAULT 0) RETURNS TABLE(tag int8, part_idx int8, res text[])
	AS 'MODULE_PATHNAME', 'sift_test_window' LANGUAGE C;
CREATE FUNCTION sift._test_window_stats(OUT rows_in int8, OUT rows_looked int8, OUT rows_out int8, OUT skips int8, OUT max_pending int8,
										OUT granules_read int8, OUT granules_total int8)
	AS 'MODULE_PATHNAME', 'sift_test_window_stats' LANGUAGE C STRICT;
-- Vectorized expressions (src/agg/vexpr_test.c): a program written as a prefix expression over the columns of a table, the live rows' results as text.
CREATE FUNCTION sift._test_vexpr(prog text, i int8[], j int8[], k int4[], x float8[], y float8[], m bool[], d jsonb[], act bool[] DEFAULT NULL,
								 n int8[] DEFAULT NULL, w numeric[] DEFAULT NULL, t timestamp[] DEFAULT NULL, dt date[] DEFAULT NULL)
	RETURNS TABLE(ord int4, res text)
	AS 'MODULE_PATHNAME', 'sift_test_vexpr' LANGUAGE C;
CREATE FUNCTION sift._test_explain_v6(scenario int, format text, verbose bool DEFAULT false) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_explain_v6' LANGUAGE C;
CREATE FUNCTION sift._test_vexpr_compiles(prog text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_test_vexpr_compiles' LANGUAGE C STRICT;
-- Grouping sets in one pass (src/agg/rollup_test.c): the groups of every set, in the columns of the GROUPING SETS query that the SQL test compares them to.
CREATE FUNCTION sift._test_rollup(ka int8[], kb int8[], kc int8[], va int8[], vb int8[], masks int8[], prehash bool DEFAULT false, batch int4 DEFAULT 2048,
								  with_distinct bool DEFAULT false, act bool[] DEFAULT NULL)
	RETURNS TABLE(set_idx int4, gset int4, k1 int8, k2 int8, k3 int8, n int8, c1 int8, s1 numeric, mn1 int8, mx1 int8, c2 int8, s2 numeric, mn2 int8,
				  mx2 int8, dcnt int8)
	AS 'MODULE_PATHNAME', 'sift_test_rollup' LANGUAGE C;
CREATE FUNCTION sift._test_rollup_distinct(masks int8[], n int8, max_bytes int8, OUT errored bool, OUT retained int8)
	AS 'MODULE_PATHNAME', 'sift_test_rollup_distinct' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_state_flags(index regclass) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_f2_state_flags' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_pack_tails(index regclass) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_f2_pack_tails' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_f2_pack_info(index regclass, part_id int8, plo int8, OUT part_gen int8, OUT header_blk int8, OUT packed bool, OUT slot int4, OUT inline bool)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_f2_pack_info' LANGUAGE C;
CREATE FUNCTION sift._test_f2_compact_racing(index regclass, part_id int8, plo int8, gens int8[], deleted_meanwhile int4) RETURNS bool AS 'MODULE_PATHNAME', 'sift_test_f2_compact_racing' LANGUAGE C;
-- The guards of the planners of the aggregation (src/agg/plan/safety.c) and the bridges of SiftAggV6 (src/agg/v6/bridge_test.c): whether a rewritten query has row security or a security barrier; the guards on
-- built nodes (the number of wrong answers); the ids of the rows the vector machine passes for a WHERE clause over the columns of the table of an index (NULL: it does not run it); what a query is as the
-- planner and the executor give it with the bridges (the plan has the node, the rows are those of the normal plan: 'ok' or what differs); the clauses the cardinality bridge made of a query.
CREATE FUNCTION sift._test_agg_guards() RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_agg_guards' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_agg_query_rls(query text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_test_agg_query_rls' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_v6_filter(idx regclass, cond text, p int8 DEFAULT NULL) RETURNS int8[]
	AS 'MODULE_PATHNAME', 'sift_test_v6_filter' LANGUAGE C;
CREATE FUNCTION sift._test_v6_check(query text, node text, bridges text[], ordered bool DEFAULT false) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_v6_check' LANGUAGE C;
CREATE FUNCTION sift._test_v6_card(query text, OUT npreds int4, OUT nrest int4, OUT replaced bool, OUT pg_rows float8, OUT rows float8, OUT calls int4)
	AS 'MODULE_PATHNAME', 'sift_test_v6_card' LANGUAGE C STRICT;

-- The queue of the background maintenance (src/core/maint_step_test.c): _test_maint_enqueue requests a step of an index of format major 2 as a trigger does (kind 3 is the compaction of a part),
-- _test_maint_stats reads the counters of the queue and how many requests it holds.
CREATE FUNCTION sift._test_maint_enqueue(index regclass, kind int4) RETURNS text AS 'MODULE_PATHNAME', 'sift_test_maint_enqueue' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_maint_stats(OUT enqueued int8, OUT coalesced int8, OUT completed int8, OUT spawned int8, OUT pending int4)
	RETURNS record AS 'MODULE_PATHNAME', 'sift_test_maint_stats' LANGUAGE C STRICT;

-- The workers sift.max_maint_workers allows are those of the queue and those of the other kinds (src/core/worker.c): _test_worker_slots(n) and _test_maint_workers_hold(n) hold n of each as running ones
-- of this backend (0 gives them back) and say how many they got, _test_maint_workers says how many of each run.
CREATE FUNCTION sift._test_worker_slots(n int4) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_worker_slots' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_maint_workers_hold(n int4) RETURNS int4 AS 'MODULE_PATHNAME', 'sift_test_maint_workers_hold' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_maint_workers(OUT other int4, OUT queue int4) RETURNS record AS 'MODULE_PATHNAME', 'sift_test_maint_workers' LANGUAGE C STRICT;

-- The flushes of a transaction's staged rows and deletes since the backend started (src/col/f2/l0_stage.c), for the tests that bound them.
CREATE FUNCTION sift._test_stage_flushes() RETURNS int8 AS 'MODULE_PATHNAME', 'sift_test_stage_flushes' LANGUAGE C STRICT;
