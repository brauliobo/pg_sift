-- The public setting surface of the 6.0 release (specs/19 section 2.2 and the four sift.gpu* settings of the Vulkan backend, which stays; TC-02):
-- 27 sift.* settings, written as the server reports them, sift.parallel_scan on by default, no setting of the 90 that 5.1 had and 6.0 removed, and none of the testing settings.
-- The testing library has more settings (the sift.test_* ones and the switches the release turns into constants): there the check of the
-- count and of the testing settings is skipped, and the 27 below are the same.

CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SELECT EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'pg_sift_test') AS testing_build \gset
CREATE TEMP TABLE release_gucs (name text PRIMARY KEY);
INSERT INTO release_gucs VALUES
	('sift.agg_exact_float'),
	('sift.async_maintenance'),
	('sift.backend_cache_kb'),
	('sift.count_refusals'),
	('sift.dead_rewrite_pct'),
	('sift.enable_agg'),
	('sift.enable_col_scan'),
	('sift.enable_scan'),
	('sift.explain_refusals'),
	('sift.filter_pushdown'),
	('sift.gpu'),
	('sift.gpu_cache_mb'),
	('sift.gpu_device'),
	('sift.gpu_min_rows'),
	('sift.log_min_duration'),
	('sift.maintenance_cost_delay'),
	('sift.maintenance_naptime'),
	('sift.maintenance_wal_rate'),
	('sift.max_expansions'),
	('sift.max_maint_workers'),
	('sift.on_corruption'),
	('sift.parallel_scan'),
	('sift.rls_scores'),
	('sift.shared_cache_kb'),
	('sift.simd'),
	('sift.standby_conflicts'),
	('sift.vacuum_rewrite_limit');
SELECT name, boot_val AS setting, boot_val FROM pg_settings
WHERE name LIKE 'sift.%' AND (NOT :'testing_build'::bool OR name IN (SELECT name FROM release_gucs))
ORDER BY name COLLATE "C";
SELECT count(*) = 27 AS the_27_settings_are_registered FROM pg_settings WHERE name IN (SELECT name FROM release_gucs);
SELECT :'testing_build'::bool OR (SELECT count(*) = 27 FROM pg_settings WHERE name LIKE 'sift.%') AS the_release_registers_only_27;
SELECT setting = 'on' AND boot_val = 'on' AS parallel_scan_is_on_by_default FROM pg_settings WHERE name = 'sift.parallel_scan';
SELECT NOT EXISTS (SELECT 1 FROM pg_settings WHERE name LIKE 'sift.test\_%' AND NOT :'testing_build'::bool) AS no_testing_setting_in_the_release;
CREATE TEMP TABLE removed_gucs (name text PRIMARY KEY);
INSERT INTO removed_gucs VALUES
	('sift.agg_combo_cache'),
	('sift.agg_dead_delta'),
	('sift.agg_dense'),
	('sift.agg_dimmap_kb'),
	('sift.agg_float_step'),
	('sift.agg_fuse'),
	('sift.agg_lookup_cache_kb'),
	('sift.agg_lookup_max_kb'),
	('sift.agg_lookup_max_rows'),
	('sift.agg_lookup_shared_kb'),
	('sift.agg_par_min_rows'),
	('sift.agg_param_set_size'),
	('sift.agg_probe_direct'),
	('sift.agg_probe_rewrite'),
	('sift.agg_rollup_cost'),
	('sift.agg_scale_horizon'),
	('sift.agg_semi_lut_size'),
	('sift.agg_super'),
	('sift.agg_two_level'),
	('sift.agg_verdict_cache'),
	('sift.cluster_advice'),
	('sift.col_auto_connect'),
	('sift.col_auto_merge'),
	('sift.col_auto_merge_buffer_pct'),
	('sift.col_auto_merge_fragmentation'),
	('sift.col_auto_merge_partition_kb'),
	('sift.col_auto_merge_pass_mb'),
	('sift.col_auto_merge_wal_pct'),
	('sift.col_connect_refresh_pct'),
	('sift.col_dict_shared_cache_kb'),
	('sift.col_format_minor'),
	('sift.col_leaf_cache_kb'),
	('sift.col_rollup_on_demand'),
	('sift.col_rollup_worker'),
	('sift.col_scope_refresh_pct'),
	('sift.col_unit_floor'),
	('sift.col_unit_rows'),
	('sift.col_value_store'),
	('sift.col_wbuf_key_range'),
	('sift.col_wbuf_ref'),
	('sift.enable_count'),
	('sift.enable_topn'),
	('sift.plan_scale'),
	('sift.prefix_cache_size'),
	('sift.scope_max_multiplicity'),
	('sift.search_chunk_sbs'),
	('sift.table_async_promote'),
	('sift.table_auto_defrag'),
	('sift.table_auto_defrag_budget'),
	('sift.table_auto_defrag_coalesce'),
	('sift.table_auto_defrag_delay'),
	('sift.table_auto_defrag_free_pct'),
	('sift.table_auto_defrag_indexes'),
	('sift.table_auto_defrag_max_active'),
	('sift.table_auto_defrag_wal_rate'),
	('sift.table_backpressure'),
	('sift.table_bulk_groups'),
	('sift.table_compression'),
	('sift.table_dead_rewrite_pct'),
	('sift.table_dict_lookup'),
	('sift.table_hot_update'),
	('sift.table_inline_promote'),
	('sift.table_key_table'),
	('sift.table_wide_rows'),
	('sift.table_write_buffer_limit'),
	('sift.tomb_log_limit'),
	('sift.topn_runs'),
	('sift.topn_walk'),
	('sift.topn_zone_keys'),
	('sift.verdict_cache_kb');
SELECT NOT EXISTS (SELECT 1 FROM pg_settings s JOIN removed_gucs r USING (name)) AS no_removed_5_1_setting_is_registered;
SET sift.col_auto_merge = on;
SET sift.table_auto_defrag = on;
SET sift.table_backpressure = on;
SELECT NOT EXISTS (SELECT 1 FROM pg_settings WHERE name LIKE 'sift.%' AND context = 'internal') AS no_internal_setting;
DROP EXTENSION pg_sift;
