EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
# SIFT_BUILD_DIR=DIR keeps the objects, their .d files and the generated headers (DIR/gen) of one build flavor under DIR, so that another flavor (SIFT_TESTING, SANITIZE,
# SIFT_HARDEN, the PostgreSQL major...) built in the same checkout never rebuilds or overwrites them; unset, everything stays beside the sources. make clean removes DIR/src and DIR/gen alone.
SIFT_OUT := $(if $(SIFT_BUILD_DIR),$(abspath $(SIFT_BUILD_DIR)))
SIFT_OBJ := $(if $(SIFT_OUT),$(SIFT_OUT)/)
OBJS = $(patsubst %.c,$(SIFT_OBJ)%.o,$(wildcard $(SRC)/*/*.c $(SRC)/*/*/*.c $(SRC)/*/*/*/*.c))
# The update script of the release in progress is built from sql/<version>/NN-<context>.sql (scripts/sql-parts.sh): edit a part, never the generated file.
SIFT_UPDATE_SQL := $(sort $(shell $(dir $(SIFT_MK))scripts/sql-parts.sh targets))
DATA = $(sort $(wildcard sql/pg_sift--*.sql) $(SIFT_UPDATE_SQL))
PGFILEDESC = "pg_sift - Fast analytics for PostgreSQL: keep your queries, just add an index. Plus BM25 full-text search"

# New SQL tests are registered automatically; the test-only patterns below apply to them too.
REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
# -MMD records header dependencies (a .d beside each object) so a header edit rebuilds its users.
# SIFT_FAST_DEBUG=1 builds with -g1 (line tables, enough for backtraces) instead of -g: smaller objects, a faster link; the optimization is the same.
# SIFT_CCACHE=auto (default): compile through ccache when it is installed; SIFT_CCACHE=0 runs the compiler itself. The checkout and the build directory are always mapped
# to fixed prefixes, so that the debug info and __FILE__ do not tell the checkouts apart or hold the path of the machine that built the library.
PG_CPPFLAGS = $(addprefix -I$(srcdir)/,$(SIFT_INCDIRS)) $(SIFT_FMV_CPPFLAGS)
# an implicit declaration is an error: gcc 13 only warns, and a function taken to return int cuts a pointer to 32 bits (an include dropped from a file whose code is built only with assertions did that)
SIFT_CCACHE ?= auto
SIFT_USE_CCACHE := $(and $(filter-out 0,$(SIFT_CCACHE)),$(shell command -v ccache 2>/dev/null))
SIFT_PREFIX_MAP = -ffile-prefix-map=$(abspath $(or $(srcdir),.))=. $(if $(SIFT_OUT),-ffile-prefix-map=$(SIFT_OUT)=build)
PG_CFLAGS = -O2 $(if $(filter-out 0,$(SIFT_FAST_DEBUG)),-g1,-g) $(SIFT_PREFIX_MAP) -ffp-contract=off -MMD -Wno-declaration-after-statement -Werror=implicit-function-declaration -pthread $(SIFT_BASE_ISA)
SHLIB_LINK += -pthread
# PGXS inherits -flto=auto from some distributions' pg_config: a link step that starts one job per core.
# Off by default; SIFT_LTO=1 enables it with at most 4 jobs. Switching it needs `make clean`.
ifdef SIFT_LTO
PG_CFLAGS += -flto=4
SHLIB_LINK += -flto=4
else
PG_CFLAGS += -fno-lto
SHLIB_LINK += -fno-lto
endif
# VULKAN=1 (or SIFT_VULKAN=1) compiles the opt-in Vulkan compute backend (docs/GPU.md): libvulkan is opened with dlopen at first use, so there is no
# link-time dependency beyond -ldl, and the Vulkan declarations and the SPIR-V are checked in (no SDK, no shader compiler). Off by default.
# Switching it needs `make clean`.
ifneq ($(or $(VULKAN),$(SIFT_VULKAN)),)
PG_CPPFLAGS += -DSIFT_VULKAN
SHLIB_LINK += -ldl
endif
EXTRA_CLEAN = $(if $(SIFT_OUT),$(SIFT_OUT)/$(SRC) $(SIFT_OUT)/gen .build-dir,$(SIFT_UPDATE_SQL) src/*/*.d src/*/*/*.d src/*/*/*/*.d .build-flavor test/results test/regression.diffs test/regression.out)
ifdef SANITIZE
PG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SHLIB_LINK += -fsanitize=address,undefined
endif

# SIFT_TESTING=1 compiles the sift._test_* functions and installs the
# test-only pg_sift_test extension; regress tests using it are skipped otherwise.
# Switching it needs `make clean`.
# The regress tests the release library cannot run: one name or % pattern per line, in byte order (LC_ALL=C sort) inside each block; a new test goes
# in its place, so that two branches adding tests merge without a conflict (scripts/check-testing-regress.sh checks it).
TESTING_REGRESS =
TESTING_REGRESS += agg_cal_paths
TESTING_REGRESS += agg_child_input_bound
TESTING_REGRESS += agg_child_warm
TESTING_REGRESS += agg_combo_cap
TESTING_REGRESS += agg_correlated_fanout
TESTING_REGRESS += agg_cte_flat_f2
TESTING_REGRESS += agg_cte_two_level_fuse
TESTING_REGRESS += agg_cte_verdict_consolidated
TESTING_REGRESS += agg_dict_blocks_lazy
TESTING_REGRESS += agg_distinct_narrow
TESTING_REGRESS += agg_facts_reread_plans
TESTING_REGRESS += agg_flat_rewrites_consolidated
TESTING_REGRESS += agg_flat_rewrites_consolidated_2
TESTING_REGRESS += agg_float_step
TESTING_REGRESS += agg_gord_group_skip_limit
TESTING_REGRESS += agg_group_hash_probe
TESTING_REGRESS += agg_group_nospill_run
TESTING_REGRESS += agg_group_passes
TESTING_REGRESS += agg_group_single_key
TESTING_REGRESS += agg_group_spill
TESTING_REGRESS += agg_group_spill_bounds
TESTING_REGRESS += agg_group_spill_limits
TESTING_REGRESS += agg_int_key_ids
TESTING_REGRESS += agg_join_flip_probe_consolidated
TESTING_REGRESS += agg_join_on_true
TESTING_REGRESS += agg_joined_args_types
TESTING_REGRESS += agg_jsonb_key_forms
TESTING_REGRESS += agg_jsonb_key_read
TESTING_REGRESS += agg_jsonb_not_exist
TESTING_REGRESS += agg_jsonb_typeof
TESTING_REGRESS += agg_jsonpath_kinds
TESTING_REGRESS += agg_key_cube_fallback
TESTING_REGRESS += agg_key_list_once
TESTING_REGRESS += agg_lateral_flatten_f2
TESTING_REGRESS += agg_limit_refusals
TESTING_REGRESS += agg_lookup_chain
TESTING_REGRESS += agg_lookup_index_keys
TESTING_REGRESS += agg_lookup_scan_memory
TESTING_REGRESS += agg_mapped_direct_values
TESTING_REGRESS += agg_mem_spill
TESTING_REGRESS += agg_outer_walks_once
TESTING_REGRESS += agg_par_blob_chunks
TESTING_REGRESS += agg_par_join_split
TESTING_REGRESS += agg_par_jsonb_unnest
TESTING_REGRESS += agg_par_partitions
TESTING_REGRESS += agg_par_pay
TESTING_REGRESS += agg_par_split
TESTING_REGRESS += agg_par_tseries_unnest
TESTING_REGRESS += agg_par_universal
TESTING_REGRESS += agg_par_window
TESTING_REGRESS += agg_partitions
TESTING_REGRESS += agg_partitions_many
TESTING_REGRESS += agg_plan_passes
TESTING_REGRESS += agg_plan_tables
TESTING_REGRESS += agg_planner_state_error
TESTING_REGRESS += agg_preagg_elem
TESTING_REGRESS += agg_preagg_minmax
TESTING_REGRESS += agg_preagg_rollup
TESTING_REGRESS += agg_probe_direct
TESTING_REGRESS += agg_probe_direct_columns
TESTING_REGRESS += agg_probe_direct_race
TESTING_REGRESS += agg_root_key_exists
TESTING_REGRESS += agg_scope_choice
TESTING_REGRESS += agg_scope_cost
TESTING_REGRESS += agg_scope_exact_share
TESTING_REGRESS += agg_scope_group_key
TESTING_REGRESS += agg_scope_key_table_choice
TESTING_REGRESS += agg_scope_live_overlay
TESTING_REGRESS += agg_scope_param_keys
TESTING_REGRESS += agg_scope_preserved
TESTING_REGRESS += agg_scope_recognition
TESTING_REGRESS += agg_scope_small_key_choice
TESTING_REGRESS += agg_scope_topn_delta
TESTING_REGRESS += agg_scope_two_indexes
TESTING_REGRESS += agg_scope_twohop_recognition
TESTING_REGRESS += agg_scope_union_group
TESTING_REGRESS += agg_scope_virtual_leaf
TESTING_REGRESS += agg_scope_walk_kept
TESTING_REGRESS += agg_semi_lut
TESTING_REGRESS += agg_semi_lut_int8
TESTING_REGRESS += agg_spec_pack
TESTING_REGRESS += agg_spill_contexts
TESTING_REGRESS += agg_text_absent_cost
TESTING_REGRESS += agg_topn_offset_bulk
TESTING_REGRESS += agg_topn_offset_ties_suite_testing
TESTING_REGRESS += agg_two_level_cell_qual
TESTING_REGRESS += agg_two_level_rows_consolidated
TESTING_REGRESS += agg_two_level_rows_consolidated_2
TESTING_REGRESS += agg_v6_adaptive_star
TESTING_REGRESS += agg_v6_bridges
TESTING_REGRESS += agg_v6_clickhouse_sql
TESTING_REGRESS += agg_v6_connect_dim_cache_f1
TESTING_REGRESS += agg_v6_const_group_empty
TESTING_REGRESS += agg_v6_core_suite
TESTING_REGRESS += agg_v6_cost_unlocks
TESTING_REGRESS += agg_v6_datum_decode_types
TESTING_REGRESS += agg_v6_datum_expr
TESTING_REGRESS += agg_v6_dim_unnest_aggs
TESTING_REGRESS += agg_v6_f1_jsonb_nonarray_elem
TESTING_REGRESS += agg_v6_f1_left_cross_dims
TESTING_REGRESS += agg_v6_f1_outer_join_rows
TESTING_REGRESS += agg_v6_float8_overflow
TESTING_REGRESS += agg_v6_generic
TESTING_REGRESS += agg_v6_hybrid_source_nan
TESTING_REGRESS += agg_v6_join_lookup
TESTING_REGRESS += agg_v6_jsonb_exists
TESTING_REGRESS += agg_v6_lateral_table_params
TESTING_REGRESS += agg_v6_live_suites
TESTING_REGRESS += agg_v6_lookup_nan
TESTING_REGRESS += agg_v6_matrix_remaining_tail
TESTING_REGRESS += agg_v6_matrix_topn_const
TESTING_REGRESS += agg_v6_numeric_nan
TESTING_REGRESS += agg_v6_pacer_foreground
TESTING_REGRESS += agg_v6_par
TESTING_REGRESS += agg_v6_par_dist_lean
TESTING_REGRESS += agg_v6_par_distinct_dict
TESTING_REGRESS += agg_v6_par_distinct_dict_robust
TESTING_REGRESS += agg_v6_par_topk_ext
TESTING_REGRESS += agg_v6_parallel_2stage
TESTING_REGRESS += agg_v6_parallel_2stage_robust
TESTING_REGRESS += agg_v6_parallel_col_delta
TESTING_REGRESS += agg_v6_params_float_exec
TESTING_REGRESS += agg_v6_parity_workload
TESTING_REGRESS += agg_v6_plan_inval
TESTING_REGRESS += agg_v6_pooler_params
TESTING_REGRESS += agg_v6_preagg_v2_oracle
TESTING_REGRESS += agg_v6_prepared_quarantine
TESTING_REGRESS += agg_v6_push_semi_keys
TESTING_REGRESS += agg_v6_recordset_nonobject
TESTING_REGRESS += agg_v6_rows_dict_text
TESTING_REGRESS += agg_v6_rows_text_ties
TESTING_REGRESS += agg_v6_rows_topn_ties
TESTING_REGRESS += agg_v6_scan_stats
TESTING_REGRESS += agg_v6_scope_topk
TESTING_REGRESS += agg_v6_security_barrier_rls
TESTING_REGRESS += agg_v6_serializable_locks
TESTING_REGRESS += agg_v6_sip_window_expr
TESTING_REGRESS += agg_v6_spill_context
TESTING_REGRESS += agg_v6_star_jsonb_unnest
TESTING_REGRESS += agg_v6_star_text
TESTING_REGRESS += agg_v6_text_collation
TESTING_REGRESS += agg_v6_text_phrase
TESTING_REGRESS += agg_v6_topn_card_vexpr
TESTING_REGRESS += agg_v6_topn_search_rows
TESTING_REGRESS += agg_v6_topn_window_limits
TESTING_REGRESS += agg_v6_tpch_exprs
TESTING_REGRESS += agg_v6_zone_calendar
TESTING_REGRESS += agg_vis_counts
TESTING_REGRESS += agg_vnum_program
TESTING_REGRESS += agg_window_topn_consolidated
TESTING_REGRESS += aggkern_%
TESTING_REGRESS += aggstub_%
TESTING_REGRESS += analyzer_version
TESTING_REGRESS += bm25_dead_windows
TESTING_REGRESS += bm25_dict_block_limit
TESTING_REGRESS += bm25_field_dl_bm25f
TESTING_REGRESS += bm25_lazy_tail_maxscore
TESTING_REGRESS += bm25_par_part_ranked
TESTING_REGRESS += bm25_tidbridge
TESTING_REGRESS += bm25_wbuf_read_promote
TESTING_REGRESS += bm25_wbuf_wide_bloom
TESTING_REGRESS += build_limits
TESTING_REGRESS += build_page_text
TESTING_REGRESS += build_pool
TESTING_REGRESS += cache_discard_all
TESTING_REGRESS += cache_eviction_safety
TESTING_REGRESS += clickbench_suite2_smoke
TESTING_REGRESS += col_advice_connect
TESTING_REGRESS += col_advice_connect_keycut
TESTING_REGRESS += col_alloc_limit
TESTING_REGRESS += col_alloc_scaled
TESTING_REGRESS += col_alter_layout
TESTING_REGRESS += col_alter_options
TESTING_REGRESS += col_approx
TESTING_REGRESS += col_auto_merge
TESTING_REGRESS += col_auto_merge_partition
TESTING_REGRESS += col_batch_bits_bounds
TESTING_REGRESS += col_build_choose
TESTING_REGRESS += col_build_memory
TESTING_REGRESS += col_build_merge_ends
TESTING_REGRESS += col_build_merge_partitions
TESTING_REGRESS += col_build_open
TESTING_REGRESS += col_build_par_choice
TESTING_REGRESS += col_build_par_rows
TESTING_REGRESS += col_build_paths
TESTING_REGRESS += col_build_same_options
TESTING_REGRESS += col_build_spool
TESTING_REGRESS += col_build_spool_values
TESTING_REGRESS += col_calendar_columns
TESTING_REGRESS += col_census_sample
TESTING_REGRESS += col_check
TESTING_REGRESS += col_conn_auto
TESTING_REGRESS += col_conn_declare
TESTING_REGRESS += col_conn_fill
TESTING_REGRESS += col_conn_found
TESTING_REGRESS += col_conn_inserted
TESTING_REGRESS += col_conn_invert
TESTING_REGRESS += col_conn_journal
TESTING_REGRESS += col_conn_journal_mem
TESTING_REGRESS += col_conn_keyset
TESTING_REGRESS += col_conn_probe
TESTING_REGRESS += col_conn_refresh
TESTING_REGRESS += col_conn_reindex
TESTING_REGRESS += col_conn_relocate
TESTING_REGRESS += col_cube_priority
TESTING_REGRESS += col_cube_rank
TESTING_REGRESS += col_cut_aligned
TESTING_REGRESS += col_damage
TESTING_REGRESS += col_dead_delta
TESTING_REGRESS += col_dead_keys_budget
TESTING_REGRESS += col_dead_rewrite
TESTING_REGRESS += col_delta_scoped
TESTING_REGRESS += col_delta_unchanged
TESTING_REGRESS += col_delvec_delta_chunks
TESTING_REGRESS += col_dict_bisect
TESTING_REGRESS += col_dict_blocks_hash
TESTING_REGRESS += col_dict_cache
TESTING_REGRESS += col_dict_count
TESTING_REGRESS += col_dict_hash
TESTING_REGRESS += col_dict_lookup
TESTING_REGRESS += col_dict_packed
TESTING_REGRESS += col_dict_shared
TESTING_REGRESS += col_dict_shared_stale
TESTING_REGRESS += col_dict_stored
TESTING_REGRESS += col_dir_lineage
TESTING_REGRESS += col_dir_nested
TESTING_REGRESS += col_dir_past_policy
TESTING_REGRESS += col_document_present
TESTING_REGRESS += col_dsm_write_caches
TESTING_REGRESS += col_dsm_write_statements
TESTING_REGRESS += col_dv_key_range
TESTING_REGRESS += col_dv_memory
TESTING_REGRESS += col_dv_oracle
TESTING_REGRESS += col_dv_reads_paid
TESTING_REGRESS += col_dv_switch
TESTING_REGRESS += col_enc
TESTING_REGRESS += col_f2_index_stats
TESTING_REGRESS += col_f2_mergejoin_mark
TESTING_REGRESS += col_f2_ordered_scan
TESTING_REGRESS += col_f2_stage_spec_abort
TESTING_REGRESS += col_f2_time_dirty_window
TESTING_REGRESS += col_fold_due_count
TESTING_REGRESS += col_fold_keys_mem
TESTING_REGRESS += col_fold_marks
TESTING_REGRESS += col_format4
TESTING_REGRESS += col_format5
TESTING_REGRESS += col_gorder
TESTING_REGRESS += col_gorder_auto
TESTING_REGRESS += col_gorder_autogroup
TESTING_REGRESS += col_gorder_autogroup_kinds
TESTING_REGRESS += col_gorder_chunks
TESTING_REGRESS += col_gorder_count_big_group
TESTING_REGRESS += col_gorder_ends
TESTING_REGRESS += col_gorder_entity_group
TESTING_REGRESS += col_gorder_free_cache
TESTING_REGRESS += col_gorder_fullrange
TESTING_REGRESS += col_gorder_gaps
TESTING_REGRESS += col_gorder_group
TESTING_REGRESS += col_gorder_group_compact
TESTING_REGRESS += col_gorder_group_count
TESTING_REGRESS += col_gorder_group_points
TESTING_REGRESS += col_gorder_group_pos
TESTING_REGRESS += col_gorder_group_suite
TESTING_REGRESS += col_gorder_group_walk
TESTING_REGRESS += col_gorder_in_walk
TESTING_REGRESS += col_gorder_layout
TESTING_REGRESS += col_gorder_mem_segment
TESTING_REGRESS += col_gorder_narrow
TESTING_REGRESS += col_gorder_seek
TESTING_REGRESS += col_gorder_spill
TESTING_REGRESS += col_gorder_text
TESTING_REGRESS += col_gorder_textperm
TESTING_REGRESS += col_gorder_tiebreak
TESTING_REGRESS += col_gorder_track_scatter
TESTING_REGRESS += col_heapfree_pending
TESTING_REGRESS += col_heapfree_stamp_words
TESTING_REGRESS += col_heapfree_xmin
TESTING_REGRESS += col_holes_fuzz
TESTING_REGRESS += col_index_%
TESTING_REGRESS += col_insert_context_lazy
TESTING_REGRESS += col_ioss_columns
TESTING_REGRESS += col_json_null_scan
TESTING_REGRESS += col_kernels_%
TESTING_REGRESS += col_key_columns_auto
TESTING_REGRESS += col_key_cut
TESTING_REGRESS += col_key_cut_big_overlays
TESTING_REGRESS += col_key_cut_btree
TESTING_REGRESS += col_key_cut_grown_overlay
TESTING_REGRESS += col_key_cut_overlap
TESTING_REGRESS += col_key_cut_overlay_half
TESTING_REGRESS += col_key_cut_overlay_mass
TESTING_REGRESS += col_key_cut_overlay_pieces
TESTING_REGRESS += col_key_cut_paced
TESTING_REGRESS += col_key_cut_partition
TESTING_REGRESS += col_key_cut_ranges
TESTING_REGRESS += col_key_cut_ranges_small
TESTING_REGRESS += col_key_cut_segment_cap
TESTING_REGRESS += col_key_cut_switch_spill
TESTING_REGRESS += col_key_cut_unrecorded
TESTING_REGRESS += col_key_cut_workers
TESTING_REGRESS += col_key_presence
TESTING_REGRESS += col_key_runs
TESTING_REGRESS += col_key_table
TESTING_REGRESS += col_leaf_fold
TESTING_REGRESS += col_leaf_stats
TESTING_REGRESS += col_list_test_once
TESTING_REGRESS += col_live_repair
TESTING_REGRESS += col_log_newer_format
TESTING_REGRESS += col_merge_dict_stream
TESTING_REGRESS += col_merge_growth
TESTING_REGRESS += col_merge_idempotent
TESTING_REGRESS += col_merge_key_cut_minimum
TESTING_REGRESS += col_merge_keys
TESTING_REGRESS += col_merge_overlay_runs
TESTING_REGRESS += col_merge_partition_setting
TESTING_REGRESS += col_merge_settled
TESTING_REGRESS += col_option_names_bytes
TESTING_REGRESS += col_order_layout
TESTING_REGRESS += col_ordered_budget
TESTING_REGRESS += col_overlay_cap
TESTING_REGRESS += col_overlay_merge
TESTING_REGRESS += col_overlay_unordered
TESTING_REGRESS += col_partition_adopt
TESTING_REGRESS += col_parts_dead_delta
TESTING_REGRESS += col_paths_auto
TESTING_REGRESS += col_paths_nested
TESTING_REGRESS += col_peers_dead_scan
TESTING_REGRESS += col_point_chunks
TESTING_REGRESS += col_point_dense
TESTING_REGRESS += col_point_dir
TESTING_REGRESS += col_point_exact
TESTING_REGRESS += col_point_lazy
TESTING_REGRESS += col_point_many_keys
TESTING_REGRESS += col_point_packed
TESTING_REGRESS += col_point_radix
TESTING_REGRESS += col_point_root
TESTING_REGRESS += col_point_suite_testing
TESTING_REGRESS += col_point_tid
TESTING_REGRESS += col_pre_census_lazy
TESTING_REGRESS += col_preagg
TESTING_REGRESS += col_preagg_calendar
TESTING_REGRESS += col_preagg_elem
TESTING_REGRESS += col_preagg_maint
TESTING_REGRESS += col_preagg_minmax
TESTING_REGRESS += col_preagg_rows
TESTING_REGRESS += col_preagg_super
TESTING_REGRESS += col_range_count
TESTING_REGRESS += col_range_count_dirty
TESTING_REGRESS += col_range_order
TESTING_REGRESS += col_read_ahead
TESTING_REGRESS += col_refresh
TESTING_REGRESS += col_reindex_from_index
TESTING_REGRESS += col_reindex_heap_reads
TESTING_REGRESS += col_rewrite_cut
TESTING_REGRESS += col_rewrite_roll
TESTING_REGRESS += col_rollup_auto
TESTING_REGRESS += col_scan_key_range
TESTING_REGRESS += col_scan_unit_order
TESTING_REGRESS += col_scope_alter_scope
TESTING_REGRESS += col_scope_auto_rollup
TESTING_REGRESS += col_scope_build
TESTING_REGRESS += col_scope_build_temp
TESTING_REGRESS += col_scope_changed_pairs
TESTING_REGRESS += col_scope_cols
TESTING_REGRESS += col_scope_cols_maint
TESTING_REGRESS += col_scope_covered
TESTING_REGRESS += col_scope_ddl_journal_suite
TESTING_REGRESS += col_scope_dead_stamp
TESTING_REGRESS += col_scope_declare
TESTING_REGRESS += col_scope_dedupe
TESTING_REGRESS += col_scope_delta
TESTING_REGRESS += col_scope_groupkey
TESTING_REGRESS += col_scope_groups
TESTING_REGRESS += col_scope_hot_chain
TESTING_REGRESS += col_scope_journal
TESTING_REGRESS += col_scope_journal_auto
TESTING_REGRESS += col_scope_journal_tags
TESTING_REGRESS += col_scope_keyed_patch
TESTING_REGRESS += col_scope_maint
TESTING_REGRESS += col_scope_manymany
TESTING_REGRESS += col_scope_manymany_edit
TESTING_REGRESS += col_scope_member_lookup
TESTING_REGRESS += col_scope_merge_end
TESTING_REGRESS += col_scope_own_command
TESTING_REGRESS += col_scope_pairs_multiset
TESTING_REGRESS += col_scope_pairs_multiset_fill
TESTING_REGRESS += col_scope_pairs_multiset_partial
TESTING_REGRESS += col_scope_pairs_multiset_twohop
TESTING_REGRESS += col_scope_pairs_partial
TESTING_REGRESS += col_scope_pairs_recorded
TESTING_REGRESS += col_scope_par_estimate
TESTING_REGRESS += col_scope_parallel
TESTING_REGRESS += col_scope_patch
TESTING_REGRESS += col_scope_patch_members
TESTING_REGRESS += col_scope_patch_price
TESTING_REGRESS += col_scope_precheck
TESTING_REGRESS += col_scope_price_terms
TESTING_REGRESS += col_scope_read
TESTING_REGRESS += col_scope_refresh_price
TESTING_REGRESS += col_scope_reindex_btree
TESTING_REGRESS += col_scope_roots_vacuum
TESTING_REGRESS += col_scope_sets
TESTING_REGRESS += col_scope_stamp
TESTING_REGRESS += col_scope_summaries
TESTING_REGRESS += col_scope_textkey
TESTING_REGRESS += col_scope_textkey_refusals
TESTING_REGRESS += col_scope_textkey_tags
TESTING_REGRESS += col_scope_textmember
TESTING_REGRESS += col_scope_textmember_collide
TESTING_REGRESS += col_scope_texttwohop
TESTING_REGRESS += col_scope_texttwohop_collide
TESTING_REGRESS += col_scope_twohop_build
TESTING_REGRESS += col_scope_twohop_choice
TESTING_REGRESS += col_scope_twohop_declare
TESTING_REGRESS += col_scope_twohop_journal
TESTING_REGRESS += col_scope_twohop_patch
TESTING_REGRESS += col_scope_twohop_tags
TESTING_REGRESS += col_scope_v6_partial_twohop
TESTING_REGRESS += col_scope_v6_refresh_race
TESTING_REGRESS += col_scope_v6_router_crossover
TESTING_REGRESS += col_scope_verify
TESTING_REGRESS += col_scope_verify_order
TESTING_REGRESS += col_scope_workers
TESTING_REGRESS += col_scope_zero_entry
TESTING_REGRESS += col_seg_head
TESTING_REGRESS += col_seg_mirror
TESTING_REGRESS += col_set_list_span
TESTING_REGRESS += col_set_pres_budget
TESTING_REGRESS += col_set_summary
TESTING_REGRESS += col_skip_index_option
TESTING_REGRESS += col_slice_aligned
TESTING_REGRESS += col_stage_expr_fn
TESTING_REGRESS += col_stmt_cid
TESTING_REGRESS += col_stmt_cid_bm25
TESTING_REGRESS += col_suggest_paths
TESTING_REGRESS += col_supersede_source
TESTING_REGRESS += col_tags
TESTING_REGRESS += col_text_contains
TESTING_REGRESS += col_text_ilike
TESTING_REGRESS += col_text_patterns
TESTING_REGRESS += col_text_prefilter
TESTING_REGRESS += col_text_scanwrap
TESTING_REGRESS += col_text_tsig
TESTING_REGRESS += col_tomb_horizon
TESTING_REGRESS += col_tomb_log
TESTING_REGRESS += col_tomb_log_block_cache
TESTING_REGRESS += col_tomb_log_cache
TESTING_REGRESS += col_tomb_stale_header
TESTING_REGRESS += col_tomb_sync_mem
TESTING_REGRESS += col_topn_arith
TESTING_REGRESS += col_topn_case
TESTING_REGRESS += col_topn_code
TESTING_REGRESS += col_topn_deep_limits
TESTING_REGRESS += col_topn_enum
TESTING_REGRESS += col_topn_expr
TESTING_REGRESS += col_topn_func
TESTING_REGRESS += col_topn_gorder
TESTING_REGRESS += col_topn_gorder_filter_text
TESTING_REGRESS += col_topn_interval
TESTING_REGRESS += col_topn_numeric
TESTING_REGRESS += col_topn_offset
TESTING_REGRESS += col_topn_raw
TESTING_REGRESS += col_topn_text
TESTING_REGRESS += col_topn_trail
TESTING_REGRESS += col_topn_unit_ends
TESTING_REGRESS += col_track_ctid
TESTING_REGRESS += col_track_ctid_chains
TESTING_REGRESS += col_track_default
TESTING_REGRESS += col_track_deletes
TESTING_REGRESS += col_track_drop_concurrently
TESTING_REGRESS += col_track_key_quoted
TESTING_REGRESS += col_track_own_statement_reads
TESTING_REGRESS += col_track_rebaseline
TESTING_REGRESS += col_track_trigger_names
TESTING_REGRESS += col_track_update_skip
TESTING_REGRESS += col_track_updates_supersede
TESTING_REGRESS += col_unique_stage
TESTING_REGRESS += col_unit_floor
TESTING_REGRESS += col_unit_of_block
TESTING_REGRESS += col_vacuum_parallel_keys
TESTING_REGRESS += col_vet_lazy_offsets
TESTING_REGRESS += col_vis_count
TESTING_REGRESS += col_volatile_notice
TESTING_REGRESS += col_wbuf
TESTING_REGRESS += col_wbuf_arena
TESTING_REGRESS += col_wbuf_arena_conn
TESTING_REGRESS += col_wbuf_arena_dbs
TESTING_REGRESS += col_wbuf_arena_format
TESTING_REGRESS += col_wbuf_arena_gc
TESTING_REGRESS += col_wbuf_arena_relocate
TESTING_REGRESS += col_wbuf_arena_shrink
TESTING_REGRESS += col_wbuf_big_set
TESTING_REGRESS += col_wbuf_cache
TESTING_REGRESS += col_wbuf_keyrange
TESTING_REGRESS += col_wbuf_order_cache
TESTING_REGRESS += col_wbuf_read_priced
TESTING_REGRESS += col_wbuf_reader_memory
TESTING_REGRESS += col_wbuf_reads_write_path
TESTING_REGRESS += col_wbuf_ref_conn
TESTING_REGRESS += col_wide_values
TESTING_REGRESS += col_wildcard_keys
TESTING_REGRESS += col_worker_failure
TESTING_REGRESS += col_writer_spool_mem
TESTING_REGRESS += col_writer_terms_mem
TESTING_REGRESS += col_zone_jnull
TESTING_REGRESS += colcodec_%
TESTING_REGRESS += compat_v1_scan_cache
TESTING_REGRESS += compat_v1_scan_dict_memory
TESTING_REGRESS += compat_v1_table_rewrite
TESTING_REGRESS += compat_v1_tomb_work_mem
TESTING_REGRESS += compat_v1_track_reindex
TESTING_REGRESS += compat_v1_vacuum_cap
TESTING_REGRESS += core_radix_sort
TESTING_REGRESS += core_range
TESTING_REGRESS += core_ring_wrap
TESTING_REGRESS += core_spool
TESTING_REGRESS += core_tidbridge
TESTING_REGRESS += errors_hints
TESTING_REGRESS += ext_recreate
TESTING_REGRESS += extlock_errors
TESTING_REGRESS += f2_check_heapallindexed
TESTING_REGRESS += f2_col_delta_unnest_pull
TESTING_REGRESS += f2_default_nan_scan
TESTING_REGRESS += f2_proj_rebuild
TESTING_REGRESS += f2_pull_prune
TESTING_REGRESS += facets_rls
TESTING_REGRESS += format2_2d_prune_aio
TESTING_REGRESS += format2_agg_live
TESTING_REGRESS += format2_arrays
TESTING_REGRESS += format2_cache_lifecycle
TESTING_REGRESS += format2_check_heap
TESTING_REGRESS += format2_col_am
TESTING_REGRESS += format2_compact
TESTING_REGRESS += format2_deep_compress
TESTING_REGRESS += format2_dv_delta_chunks
TESTING_REGRESS += format2_ext_safety
TESTING_REGRESS += format2_float_zones
TESTING_REGRESS += format2_flush_slab_turnover
TESTING_REGRESS += format2_framed_chunks
TESTING_REGRESS += format2_fsm
TESTING_REGRESS += format2_heapref_l0_wal
TESTING_REGRESS += format2_index_format_compat
TESTING_REGRESS += format2_inspect_sql
TESTING_REGRESS += format2_inval_truncate
TESTING_REGRESS += format2_key_order
TESTING_REGRESS += format2_limits
TESTING_REGRESS += format2_limits_ios
TESTING_REGRESS += format2_marks_prewhere
TESTING_REGRESS += format2_meta_marks_suite
TESTING_REGRESS += format2_mutation
TESTING_REGRESS += format2_mvcc_churn
TESTING_REGRESS += format2_on63c_parity
TESTING_REGRESS += format2_parallel_conc
TESTING_REGRESS += format2_partitioned_agg_v6
TESTING_REGRESS += format2_point_v2
TESTING_REGRESS += format2_partitioned_only
TESTING_REGRESS += format2_projections
TESTING_REGRESS += format2_prune
TESTING_REGRESS += format2_quarantine
TESTING_REGRESS += format2_reloptions_deprecated
TESTING_REGRESS += format2_scope_live
TESTING_REGRESS += format2_size
TESTING_REGRESS += format2_status_heapref_slab
TESTING_REGRESS += format2_stor_mem_col
TESTING_REGRESS += format2_storage_codec_suite
TESTING_REGRESS += format2_tiering_col
TESTING_REGRESS += format2_tiering_ttl
TESTING_REGRESS += format2_topn_tied_first_key
TESTING_REGRESS += format2_ux_progress
TESTING_REGRESS += format2_zero_unsupported
TESTING_REGRESS += format2_zero_unsupported_nan
TESTING_REGRESS += format_v1_v2_compat
TESTING_REGRESS += fuzzy_seek
TESTING_REGRESS += gpu_agg_v6_churn
TESTING_REGRESS += gpu_agg_v6_dict
TESTING_REGRESS += gpu_topn_v6
TESTING_REGRESS += hybrid_agg
TESTING_REGRESS += hybrid_agg_cost
TESTING_REGRESS += hybrid_agg_score
TESTING_REGRESS += hybrid_agg_table
TESTING_REGRESS += hybrid_multimodal_facet
TESTING_REGRESS += hybrid_or
TESTING_REGRESS += hybrid_or_direct_agg
TESTING_REGRESS += hybrid_tidbridge_bm25
TESTING_REGRESS += include_filter
TESTING_REGRESS += include_store
TESTING_REGRESS += index_build_postings
TESTING_REGRESS += index_build_thresholds
TESTING_REGRESS += index_parts
TESTING_REGRESS += join_push_parity
TESTING_REGRESS += join_push_table_parity
TESTING_REGRESS += kernels_%
TESTING_REGRESS += kll_nonfinite
TESTING_REGRESS += kll_quantiles
TESTING_REGRESS += lang_chain
TESTING_REGRESS += lang_memo
TESTING_REGRESS += leak_planted
TESTING_REGRESS += limits_registry
TESTING_REGRESS += m4_cache_budget
TESTING_REGRESS += maint_%
TESTING_REGRESS += merge_heal
TESTING_REGRESS += merge_parts
TESTING_REGRESS += on_scaling_guards
TESTING_REGRESS += p98_decoders
TESTING_REGRESS += p98_check_blockmap
TESTING_REGRESS += p98_wide_part
TESTING_REGRESS += p98_inspect_tiered
TESTING_REGRESS += p98_shm_vm_uniq_%
TESTING_REGRESS += p98_temp_shm
TESTING_REGRESS += p98_tier_orphans
TESTING_REGRESS += p98_tier_policy_cache
TESTING_REGRESS += p98_tier_privileges
TESTING_REGRESS += p98_tier_wide
TESTING_REGRESS += p98_autottl_rekey
TESTING_REGRESS += p98_ddl_catalog_sync
TESTING_REGRESS += p98_journal_ddl
TESTING_REGRESS += p98_proj_ddl_acid
TESTING_REGRESS += p98_proj_tiered
TESTING_REGRESS += p98_scope_ddl
TESTING_REGRESS += p98_ttl_drop_acid
TESTING_REGRESS += p98_sql_security
TESTING_REGRESS += par_core_pool
TESTING_REGRESS += par_core_pool_limits
TESTING_REGRESS += part_agg_parity
TESTING_REGRESS += position_blocks
TESTING_REGRESS += prod95_query_stream_join
TESTING_REGRESS += proj_%
TESTING_REGRESS += quarantine_bm25
TESTING_REGRESS += quarantine_build
TESTING_REGRESS += quarantine_capacity
TESTING_REGRESS += quarantine_col
TESTING_REGRESS += quarantine_damage
TESTING_REGRESS += quarantine_errors
TESTING_REGRESS += quarantine_hints
TESTING_REGRESS += quarantine_inject
TESTING_REGRESS += quarantine_join
TESTING_REGRESS += quarantine_parallel
TESTING_REGRESS += quarantine_partitions
TESTING_REGRESS += quarantine_persist
TESTING_REGRESS += quarantine_random
TESTING_REGRESS += quarantine_silent
TESTING_REGRESS += quarantine_track
TESTING_REGRESS += quarantine_txstate
TESTING_REGRESS += read_%
TESTING_REGRESS += scaling_ops_gate
TESTING_REGRESS += scope_in5_parity
TESTING_REGRESS += scope_overlay_parity
TESTING_REGRESS += scope_pair_edit_live
TESTING_REGRESS += scope_topn_parity
TESTING_REGRESS += scope_topn_ties
TESTING_REGRESS += scope_window_parity
TESTING_REGRESS += segfmt_%
TESTING_REGRESS += shm_ledger
TESTING_REGRESS += shm_manifest
TESTING_REGRESS += shred_%
TESTING_REGRESS += slot_owner
TESTING_REGRESS += stor_f2_col_delta_chain
TESTING_REGRESS += stor_f2_vacuum_horizon
TESTING_REGRESS += stor_v1_vacuum_truncate
TESTING_REGRESS += store_%
TESTING_REGRESS += store_sweep_runs
TESTING_REGRESS += surface_null_args
TESTING_REGRESS += table_f2_col_delta
TESTING_REGRESS += table_f2_col_delta_dir
TESTING_REGRESS += table_f2_col_delta_hot
TESTING_REGRESS += table_f2_col_delta_summ
TESTING_REGRESS += table_f2_col_delta_trigger
TESTING_REGRESS += table_f2_copy_schema_evol
TESTING_REGRESS += table_f2_dml_cache_budget
TESTING_REGRESS += table_f2_drop_in_txn
TESTING_REGRESS += table_f2_drop_parts_before
TESTING_REGRESS += table_f2_fast_fetch
TESTING_REGRESS += table_f2_fetch
TESTING_REGRESS += table_f2_join_parts
TESTING_REGRESS += table_f2_key_order_check
TESTING_REGRESS += table_f2_lock_escalation
TESTING_REGRESS += table_f2_native_strings_legacy
TESTING_REGRESS += table_f2_parity
TESTING_REGRESS += table_f2_pgstat
TESTING_REGRESS += table_f2_plans
TESTING_REGRESS += table_f2_row_pack
TESTING_REGRESS += table_f2_scan_pacer
TESTING_REGRESS += table_f2_tid_range
TESTING_REGRESS += table_f2_unified_parity
TESTING_REGRESS += table_f2_update_indexed
TESTING_REGRESS += table_fast_paths
TESTING_REGRESS += tbl_bigtx
TESTING_REGRESS += tbl_bitmap_lazy
TESTING_REGRESS += tbl_bulk_eligible
TESTING_REGRESS += tbl_fetch_dict
TESTING_REGRESS += tbl_hot_sift_index
TESTING_REGRESS += tbl_key_table
TESTING_REGRESS += tbl_limits
TESTING_REGRESS += tbl_limits_lowered
TESTING_REGRESS += tbl_options
TESTING_REGRESS += tbl_page_corrupt
TESTING_REGRESS += tbl_preagg_calendar
TESTING_REGRESS += tbl_tomb_legacy_header
TESTING_REGRESS += tbl_tomb_log_limit
TESTING_REGRESS += tbl_vacuum_batches
TESTING_REGRESS += tbl_vacuum_page_rows
TESTING_REGRESS += tbl_wide_dict_limit
TESTING_REGRESS += tbl_wide_rows
TESTING_REGRESS += tbl_writepath
TESTING_REGRESS += v1_unified_datum_fold
TESTING_REGRESS += v1_unified_gorder_cursor
TESTING_REGRESS += v1_unified_jsonb_leaf
TESTING_REGRESS += v1_unified_jsonb_member
TESTING_REGRESS += v6_partitions_attach_detach
TESTING_REGRESS += verify_%
# these assert the segments of a parallel build of an index with a key cut, which the release library builds in one process (build_pays in src/col/v1/build_par.c)
TESTING_REGRESS += agg_numeric_in_list
TESTING_REGRESS += agg_prepass_const_list
TESTING_REGRESS += agg_shape_walks
TESTING_REGRESS += agg_shape_widths
TESTING_REGRESS += agg_source_cache
TESTING_REGRESS += col_cost_keys_sorted_once
TESTING_REGRESS += col_cost_selectivity_once
# Listed although scripts/check-testing-regress.sh would not select them (each fails on the release library, so the stock-library jobs must not run it):
# sift.test_run_docs, a setting only the testing build has
TESTING_REGRESS += build_memory
TESTING_REGRESS += build_parallel
TESTING_REGRESS += build_runs
# sift.test_par_workers, a setting only the testing build has
TESTING_REGRESS += parallel_build
TESTING_REGRESS += tbl_bulk_parallel
# sift.test_range_pieces and sift.test_range_bytes, settings only the testing build has
TESTING_REGRESS += build_ranges
TESTING_REGRESS += merge_ranges
# sift.test_page_text, a setting only the testing build has
TESTING_REGRESS += build_large_docs
# sift.grouped_min_df, a setting only the testing build has
TESTING_REGRESS += list_slice_seek
TESTING_REGRESS += rank_or_union
TESTING_REGRESS += rank_page_live
# sift.test_rank_mode, a setting only the testing build has
TESTING_REGRESS += rank_modes
# sift.table_max_segments and sift.table_vblock_floor, settings only the testing build has
TESTING_REGRESS += tbl_bulk_groups
TESTING_REGRESS += tbl_cap
TESTING_REGRESS += tbl_vblocks
TESTING_REGRESS += tbl_verify_frozen
# an asserted value differs on the release library
TESTING_REGRESS += agg_count_not_null
TESTING_REGRESS += agg_preagg
TESTING_REGRESS += agg_preagg_distinct
TESTING_REGRESS += agg_preagg_linear
TESTING_REGRESS += agg_preagg_rows
TESTING_REGRESS += agg_privileges
TESTING_REGRESS += agg_refusals
TESTING_REGRESS += agg_set_array
TESTING_REGRESS += agg_unstorable_values
TESTING_REGRESS += agg_window
TESTING_REGRESS += cache_discard
TESTING_REGRESS += col_build_open_memory
TESTING_REGRESS += col_build_peers
TESTING_REGRESS += col_build_peers_unequal
TESTING_REGRESS += col_build_spool_memory
TESTING_REGRESS += col_merge_memory
TESTING_REGRESS += col_scan
TESTING_REGRESS += facet_counts
TESTING_REGRESS += roles_ddl
TESTING_REGRESS += roles_dml_query
TESTING_REGRESS += roles_maintenance
TESTING_REGRESS += tbl_agg
# sift._test_s55_* functions, only the testing build has
TESTING_REGRESS += s55_granule_point
TESTING_REGRESS += s55_point_dir
# the tests of the Format 1 settings that sift.test_* settings replaced (the testing build has them, the release library has not)
TESTING_REGRESS += agg_combo_hash
TESTING_REGRESS += agg_cte_inline_band
TESTING_REGRESS += agg_cte_inline_rows
TESTING_REGRESS += agg_dense
TESTING_REGRESS += agg_distinct_cost
TESTING_REGRESS += agg_few_keys_handful_units
TESTING_REGRESS += agg_inner_semi
TESTING_REGRESS += agg_join_key_types
TESTING_REGRESS += agg_lookup_gang
TESTING_REGRESS += agg_lookup_in_keyed
TESTING_REGRESS += agg_lookup_in_list
TESTING_REGRESS += agg_lookup_keyed_crossover
TESTING_REGRESS += agg_lookup_range_bounds
TESTING_REGRESS += agg_lookup_tests
TESTING_REGRESS += agg_par_cost
TESTING_REGRESS += agg_plan_facts_cache
TESTING_REGRESS += agg_plan_scale
TESTING_REGRESS += agg_preagg_dimfilter
TESTING_REGRESS += agg_preagg_elem_parallel
TESTING_REGRESS += agg_preagg_exceptions
TESTING_REGRESS += agg_preagg_filter
TESTING_REGRESS += agg_preagg_int8
TESTING_REGRESS += agg_preagg_measures
TESTING_REGRESS += agg_preagg_super
TESTING_REGRESS += agg_preagg_wide_key
TESTING_REGRESS += agg_probe_direct_slot
TESTING_REGRESS += agg_probe_set_pricing
TESTING_REGRESS += agg_rollup_cost
TESTING_REGRESS += agg_rows
TESTING_REGRESS += agg_scalar_initplan
TESTING_REGRESS += agg_scan_price
TESTING_REGRESS += agg_stats
TESTING_REGRESS += agg_visibility_refusal
TESTING_REGRESS += agg_workers_guessed_keys
TESTING_REGRESS += col_key_cut_advice
TESTING_REGRESS += col_merge_levels
TESTING_REGRESS += col_net_agg
TESTING_REGRESS += col_overlay_lean
TESTING_REGRESS += col_overlay_wide_units
TESTING_REGRESS += col_search
TESTING_REGRESS += col_settings_surface
TESTING_REGRESS += col_topn
TESTING_REGRESS += col_topn_order
TESTING_REGRESS += col_topn_parallel
TESTING_REGRESS += tbl_lists_runs
TESTING_REGRESS += tbl_preagg_minmax
TESTING_REGRESS += tbl_topn
TESTING_REGRESS += v1_store_ops
TESTING_REGRESS += v1_store_ops_ext
TESTING_REGRESS += v1_store_ops_par_zone
TESTING_REGRESS += v1_unified_jsonb_typeof
TESTING_REGRESS += v1_unified_preagg_cube
TESTING_REGRESS += v1_unified_rows_scankey
TESTING_REGRESS += v1_unified_scan
TESTING_REGRESS += v1_unified_star_keys
TESTING_REGRESS += v1_unified_text_rows
TESTING_REGRESS += v1_unified_topn_rows
ifdef SIFT_SB_SHIFT
ifndef SIFT_TESTING
$(error SIFT_SB_SHIFT changes the on-disk geometry: for SIFT_TESTING=1 test flavours only)
endif
PG_CPPFLAGS += -DSIFT_SB_SHIFT=$(SIFT_SB_SHIFT)
endif
ifdef SIFT_TESTING
EXTENSION += pg_sift_test
DATA += sql/pg_sift_test--1.0.sql sql/pg_sift_test--1.0--1.1.sql
PG_CPPFLAGS += -DSIFT_TESTING
else
REGRESS := $(filter-out $(TESTING_REGRESS),$(REGRESS))
endif
# The SQL helpers of the regression tests (sql/pg_sift_helpers--1.0.sql, SQL only): the test build installs them, and so does scripts/release-library-check.sh, which runs the
# same tests on the release library (SIFT_TEST_HELPERS=1). A normal make install does not.
ifneq ($(or $(SIFT_TESTING),$(SIFT_TEST_HELPERS)),)
EXTENSION += pg_sift_helpers
DATA += sql/pg_sift_helpers--1.0.sql
endif

# No LLVM bitcode: avoids a clang dependency and -march=native bitcode.
override with_llvm := no

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs 2>/dev/null)
PG_MAJOR := $(shell $(PG_CONFIG) --version 2>/dev/null | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
ifeq ($(PG_MAJOR),)
$(error cannot run '$(PG_CONFIG)': install the PostgreSQL server development files or set PG_CONFIG=/path/to/pg_config)
endif
PG_MIN_MAJOR = 17
ifeq ($(shell test $(PG_MAJOR) -ge $(PG_MIN_MAJOR) && echo ok),)
$(error pg_sift needs PostgreSQL $(PG_MIN_MAJOR) or newer, but '$(PG_CONFIG)' is PostgreSQL $(PG_MAJOR))
endif

# The general-purpose layers of the column codec (src/col/compress.c): LZ4 and zstd, each linked; a header or library that is not found stops the build.
# SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 build without one: chunks never use it, and one written elsewhere that does fails to open with a clear error.
# pg_config's flags carry the include and library directories of a package manager (Homebrew keeps lz4 and zstd out of the compiler's default paths)
colz_flags := $(shell $(PG_CONFIG) --cppflags --ldflags)
define colz_probe
$(if $(SIFT_NO_$(1)),,$(if $(shell echo 'int main(void) { return 0; }' | $(CC) $(colz_flags) -include $(2).h -x c - -l$(2) -o /dev/null 2>/dev/null && echo ok),$(eval PG_CPPFLAGS += -DSIFT_HAVE_$(1))$(eval SHLIB_LINK += -l$(2)),$(error pg_sift: $(2) not found (install lib$(2) headers, or build with SIFT_NO_$(1)=1))))
endef
$(call colz_probe,LZ4,lz4)
$(call colz_probe,ZSTD,zstd)

# SIFT_HARDEN=pkg|init|1: opt-in compiler hardening (scripts/harden.mk, docs/SAFETY.md)
include $(dir $(SIFT_MK))scripts/harden.mk

# make check only runs scripts/ci.sh, which builds and installs on its own; PGXS would
# claim "check" is unsupported, so it is not included for these goals. pg-assert and pg-valgrind build the PostgreSQL of the `assert`
# and `valgrind` CI lanes once into a cache outside the repo (scripts/pg-assert.sh; PG_ASSERT_MAJOR=17 for the 17 branch).
ifneq ($(filter check lint dev-check check-scaling-ops pg-assert pg-valgrind,$(MAKECMDGOALS)),)
.PHONY: check lint dev-check check-scaling-ops pg-assert pg-valgrind
check:
	scripts/ci.sh quick
lint:
	scripts/lint.sh
dev-check:
	scripts/lint.sh complexity memory locks capacity file-size neutral format-isolation
check-scaling-ops:
	scripts/check-scaling-ops.sh
pg-assert:
	nice scripts/pg-assert.sh $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
pg-valgrind:
	nice scripts/pg-assert.sh --valgrind $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
else
include $(PGXS)

# SIFT_LLD=auto (default): link with ld.lld on Linux when the compiler can use it and SIFT_LTO is off; SIFT_LLD=0 keeps the system linker.
SIFT_LLD ?= auto
SIFT_USE_LLD := $(and $(filter Linux,$(shell uname -s)),$(filter-out 0,$(SIFT_LLD)),$(if $(SIFT_LTO),,x),$(shell command -v ld.lld 2>/dev/null),$(shell echo 'int main(void) { return 0; }' | $(CC) -fuse-ld=lld -x c - -o /dev/null 2>/dev/null && echo ok))
SHLIB_LINK += $(if $(SIFT_USE_LLD),-fuse-ld=lld)
ifneq ($(and $(SIFT_USE_CCACHE),$(if $(findstring ccache,$(CC)),,x)),)
override CC := ccache $(CC)
endif

ifneq ($(SIFT_OUT),)
$(OBJS): $(SIFT_OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(COMPILE.c) $(OUTPUT_OPTION) $<
# pg_sift$(DLSUFFIX) stays beside the sources, where PGXS links and installs it: it is relinked (from the objects of SIFT_BUILD_DIR, nothing is compiled) whenever another build directory linked it last
.build-dir: FORCE
	@[ "$$(cat $@ 2>/dev/null)" = '$(SIFT_OUT)' ] || echo '$(SIFT_OUT)' >$@
$(shlib): .build-dir
endif
endif

.PHONY: update-sql
$(SIFT_UPDATE_SQL): $(wildcard sql/[0-9]*/*.sql) scripts/sql-parts.sh
	scripts/sql-parts.sh build
update-sql: $(SIFT_UPDATE_SQL)
all: update-sql

-include $(wildcard $(OBJS:.o=.d))

# The module version the library reports (scripts/build-id.sh); build_id.h is rewritten only when the id changes, so that handler.o is rebuilt then and no other file.
SIFT_BUILD_ID ?= $(shell $(srcdir)/scripts/build-id.sh $(or $(srcdir),.))
SIFT_GEN := $(if $(SIFT_OUT),$(SIFT_OUT)/gen,$(SRC))
$(SIFT_GEN)/core/build_id.h: FORCE
	@mkdir -p $(@D); echo '#define SIFT_BUILD_ID "$(SIFT_BUILD_ID)"' >$@.new; cmp -s $@.new $@ || mv $@.new $@; rm -f $@.new
$(SIFT_OBJ)$(SRC)/core/handler.o: $(SIFT_GEN)/core/build_id.h
# The identity of the sources and of the options that change them (scripts/code-id.sh): the shared tables of a server are told apart by it (col/dsmreg.c); rewritten only when it changes, so that only dsmreg.o is rebuilt then.
SIFT_CODE_ID = $(shell $(srcdir)/scripts/code-id.sh $(or $(srcdir),.) '$(filter -DSIFT_%,$(PG_CPPFLAGS))')
$(SIFT_GEN)/core/code_id.h: FORCE
	@mkdir -p $(@D); echo '#define SIFT_CODE_ID "$(SIFT_CODE_ID)"' >$@.new; cmp -s $@.new $@ || mv $@.new $@; rm -f $@.new
$(SIFT_OBJ)$(SRC)/col/dsmreg.o: $(SIFT_GEN)/core/code_id.h
# the two files that include them look in the build directory first: a header another build left in src/core is never read
ifneq ($(SIFT_OUT),)
$(SIFT_OBJ)$(SRC)/core/handler.o $(SIFT_OBJ)$(SRC)/col/dsmreg.o: override CPPFLAGS := -I$(SIFT_GEN) $(CPPFLAGS)
endif
.PHONY: FORCE
FORCE:

# Suites beyond installcheck, run against the scripts/pgtest.sh cluster
# (install and start it first). scripts/ci.sh runs all of them.
.PHONY: isolationcheck tapcheck difftest fuzz soak gpu-shaders limits
isolationcheck:
	scripts/isolation.sh
tapcheck:
	scripts/tap.sh
difftest fuzz soak:
	scripts/$@.sh
# the x86-64 baseline of the objects and of pg_sift.so, by symbol (scripts/isa_check.sh)
.PHONY: isa-check
isa-check: all
	scripts/isa_check.sh
# docs/LIMITS.md from the registry in src/core/limits.h (scripts/gen-limits.sh --check proves it)
limits:
	scripts/gen-limits.sh

# Rebuilds src/agg/vk_spv.h from the shaders of src/agg/vk/shaders (needs glslc) and the Vulkan declarations from /usr/include/vulkan; commit the result.
gpu-shaders:
	scripts/gen-aggvk-spv.sh
	scripts/gen-aggvk-min.py
