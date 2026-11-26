EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
# SIFT_BUILD_DIR=DIR keeps the objects, their .d files and the generated headers (DIR/gen) of one build flavor under DIR, so that another flavor (SIFT_TESTING, SANITIZE,
# SIFT_HARDEN, the PostgreSQL major...) built in the same checkout never rebuilds or overwrites them; unset, everything stays beside the sources. make clean removes DIR/src and DIR/gen alone.
SIFT_OUT := $(if $(SIFT_BUILD_DIR),$(abspath $(SIFT_BUILD_DIR)))
SIFT_OBJ := $(if $(SIFT_OUT),$(SIFT_OUT)/)
OBJS = $(patsubst %.c,$(SIFT_OBJ)%.o,$(wildcard $(SRC)/*/*.c $(SRC)/*/*/*.c $(SRC)/*/*/*/*.c $(SRC)/*/*/*/*/*.c))
# The update script of the release in progress is built from sql/<version>/NN-<context>.sql (scripts/sql-parts.sh): edit a part, never the generated file.
SIFT_UPDATE_SQL := $(sort $(shell $(dir $(SIFT_MK))scripts/sql-parts.sh targets))
DATA = $(sort $(wildcard sql/pg_sift--*.sql) $(SIFT_UPDATE_SQL))
PGFILEDESC = "pg_sift - Fast analytics for PostgreSQL: keep your queries, just add an index. Plus BM25 full-text search"

# New SQL tests are registered automatically; the test-only patterns below apply to them too.
REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql test/sql/*/*.sql test/sql/*/*/*.sql test/sql/*/*/*/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test
REGRESS_PREP = regress-results-dirs
.DEFAULT_GOAL := all
.PHONY: regress-results-dirs
regress-results-dirs: ; @mkdir -p $(sort $(addprefix test/results/,$(dir $(REGRESS))))

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
EXTRA_CLEAN = $(if $(SIFT_OUT),$(SIFT_OUT)/$(SRC) $(SIFT_OUT)/gen .build-dir,$(SIFT_UPDATE_SQL) .sql-parts.stamp src/*/*.d src/*/*/*.d src/*/*/*/*.d src/*/*/*/*/*.d .build-flavor test/results test/regression.diffs test/regression.out)
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
TESTING_REGRESS += agg/adaptive_star
TESTING_REGRESS += agg/bridges
TESTING_REGRESS += agg/cal_paths
TESTING_REGRESS += agg/child/input_bound
TESTING_REGRESS += agg/clickbench_suite2_smoke
TESTING_REGRESS += agg/clickhouse_sql
TESTING_REGRESS += agg/combo/cap
TESTING_REGRESS += agg/connect_dim_cache_f1
TESTING_REGRESS += agg/const_group_empty
TESTING_REGRESS += agg/core_suite
TESTING_REGRESS += agg/correlated/fanout
TESTING_REGRESS += agg/cost_unlocks
TESTING_REGRESS += agg/cte/flat_f2
TESTING_REGRESS += agg/cte/two_level_fuse
TESTING_REGRESS += agg/datum_decode_types
TESTING_REGRESS += agg/datum_expr
TESTING_REGRESS += agg/dict/blocks_lazy
TESTING_REGRESS += agg/dim_unnest_aggs
TESTING_REGRESS += agg/distinct/narrow
TESTING_REGRESS += agg/f1_jsonb_nonarray_elem
TESTING_REGRESS += agg/f1_left_cross_dims
TESTING_REGRESS += agg/f1_outer_join_rows
TESTING_REGRESS += agg/facts_reread_plans
TESTING_REGRESS += agg/flat_rewrites_consolidated
TESTING_REGRESS += agg/flat_rewrites_consolidated_2
TESTING_REGRESS += agg/float8_overflow
TESTING_REGRESS += agg/generic
TESTING_REGRESS += agg/gord_group_skip_limit
TESTING_REGRESS += agg/group/nospill_run
TESTING_REGRESS += agg/group/spill_bounds
TESTING_REGRESS += agg/hybrid/basic
TESTING_REGRESS += agg/hybrid/cost
TESTING_REGRESS += agg/hybrid/multimodal_facet
TESTING_REGRESS += agg/hybrid/or
TESTING_REGRESS += agg/hybrid/or_direct
TESTING_REGRESS += agg/hybrid/score
TESTING_REGRESS += agg/hybrid/table
TESTING_REGRESS += agg/hybrid/tidbridge_bm25
TESTING_REGRESS += agg/hybrid_source_nan
TESTING_REGRESS += agg/join/flip_probe_consolidated
TESTING_REGRESS += agg/join/parity
TESTING_REGRESS += agg/join/table_parity
TESTING_REGRESS += agg/join/true
TESTING_REGRESS += agg/join_lookup
TESTING_REGRESS += agg/joined_args_types
TESTING_REGRESS += agg/jsonb/key_forms
TESTING_REGRESS += agg/jsonb/key_read
TESTING_REGRESS += agg/jsonb/not_exist
TESTING_REGRESS += agg/jsonb/typeof
TESTING_REGRESS += agg/jsonb_exists
TESTING_REGRESS += agg/jsonpath_kinds
TESTING_REGRESS += agg/key/cube_fallback
TESTING_REGRESS += agg/kll/nonfinite
TESTING_REGRESS += agg/kll/quantiles
TESTING_REGRESS += agg/lateral_flatten_f2
TESTING_REGRESS += agg/lateral_table_params
TESTING_REGRESS += agg/limit_refusals
TESTING_REGRESS += agg/live_suites
TESTING_REGRESS += agg/lookup/chain
TESTING_REGRESS += agg/lookup/index_keys
TESTING_REGRESS += agg/lookup/scan_memory
TESTING_REGRESS += agg/lookup_nan
TESTING_REGRESS += agg/mapped_direct_values
TESTING_REGRESS += agg/matrix_remaining_tail
TESTING_REGRESS += agg/matrix_topn_const
TESTING_REGRESS += agg/mem_spill
TESTING_REGRESS += agg/numeric_nan
TESTING_REGRESS += agg/pacer_foreground
TESTING_REGRESS += agg/par/basic
TESTING_REGRESS += agg/par/blob_chunks
TESTING_REGRESS += agg/par/dist_lean
TESTING_REGRESS += agg/par/distinct_dict
TESTING_REGRESS += agg/par/distinct_dict_robust
TESTING_REGRESS += agg/par/jsonb_unnest
TESTING_REGRESS += agg/par/partitions
TESTING_REGRESS += agg/par/topk_ext
TESTING_REGRESS += agg/par/tseries_unnest
TESTING_REGRESS += agg/par/universal
TESTING_REGRESS += agg/par/window
TESTING_REGRESS += agg/par_min_rows
TESTING_REGRESS += agg/parallel_2stage
TESTING_REGRESS += agg/parallel_2stage_robust
TESTING_REGRESS += agg/parallel_col_delta
TESTING_REGRESS += agg/params_float_exec
TESTING_REGRESS += agg/parity_workload
TESTING_REGRESS += agg/part/parity
TESTING_REGRESS += agg/partitions
TESTING_REGRESS += agg/partitions_many
TESTING_REGRESS += agg/plan/tables
TESTING_REGRESS += agg/plan_inval
TESTING_REGRESS += agg/pooler_params
TESTING_REGRESS += agg/preagg/minmax
TESTING_REGRESS += agg/preagg_v2
TESTING_REGRESS += agg/preagg_v2_oracle
TESTING_REGRESS += agg/prepared_quarantine
TESTING_REGRESS += agg/push_semi_keys
TESTING_REGRESS += agg/query_stream_join
TESTING_REGRESS += agg/recordset_nonobject
TESTING_REGRESS += agg/root_key_exists
TESTING_REGRESS += agg/rows_dict_text
TESTING_REGRESS += agg/rows_text_ties
TESTING_REGRESS += agg/rows_topn_ties
TESTING_REGRESS += agg/scaling_guards
TESTING_REGRESS += agg/scan_stats
TESTING_REGRESS += agg/scope/choice
TESTING_REGRESS += agg/scope/cost
TESTING_REGRESS += agg/scope/exact_share
TESTING_REGRESS += agg/scope/group_key
TESTING_REGRESS += agg/scope/in5_parity
TESTING_REGRESS += agg/scope/key_table_choice
TESTING_REGRESS += agg/scope/live_overlay
TESTING_REGRESS += agg/scope/overlay_parity
TESTING_REGRESS += agg/scope/pair_edit_live
TESTING_REGRESS += agg/scope/param_keys
TESTING_REGRESS += agg/scope/preserved
TESTING_REGRESS += agg/scope/recognition
TESTING_REGRESS += agg/scope/small_key_choice
TESTING_REGRESS += agg/scope/topn_delta
TESTING_REGRESS += agg/scope/topn_parity
TESTING_REGRESS += agg/scope/topn_ties
TESTING_REGRESS += agg/scope/two_indexes
TESTING_REGRESS += agg/scope/twohop_recognition
TESTING_REGRESS += agg/scope/union_group
TESTING_REGRESS += agg/scope/virtual_leaf
TESTING_REGRESS += agg/scope/walk_kept
TESTING_REGRESS += agg/scope/window_parity
TESTING_REGRESS += agg/scope_topk
TESTING_REGRESS += agg/security_barrier_rls
TESTING_REGRESS += agg/serializable_locks
TESTING_REGRESS += agg/sip_window_expr
TESTING_REGRESS += agg/spill/contexts
TESTING_REGRESS += agg/spill/group
TESTING_REGRESS += agg/spill/tight_work_mem
TESTING_REGRESS += agg/spill_context
TESTING_REGRESS += agg/star_jsonb_unnest
TESTING_REGRESS += agg/star_text
TESTING_REGRESS += agg/table_jsonb
TESTING_REGRESS += agg/text_absent_cost
TESTING_REGRESS += agg/text_collation
TESTING_REGRESS += agg/text_phrase
TESTING_REGRESS += agg/topn/offset_bulk
TESTING_REGRESS += agg/topn_card_vexpr
TESTING_REGRESS += agg/topn_search_rows
TESTING_REGRESS += agg/topn_window_limits
TESTING_REGRESS += agg/tpch_exprs
TESTING_REGRESS += agg/two_level_cell_qual
TESTING_REGRESS += agg/two_level_rows_consolidated
TESTING_REGRESS += agg/two_level_rows_consolidated_2
TESTING_REGRESS += agg/vis_counts
TESTING_REGRESS += agg/vk/churn
TESTING_REGRESS += agg/vk/dict
TESTING_REGRESS += agg/vk/topn
TESTING_REGRESS += agg/wide/batch_cols
TESTING_REGRESS += agg/window/topn_consolidated
TESTING_REGRESS += agg/zone_calendar
TESTING_REGRESS += bm25/analyze/version
TESTING_REGRESS += bm25/build/limits
TESTING_REGRESS += bm25/build/page_text
TESTING_REGRESS += bm25/build/pool
TESTING_REGRESS += bm25/dead_windows
TESTING_REGRESS += bm25/dict_block_limit
TESTING_REGRESS += bm25/facets_rls
TESTING_REGRESS += bm25/field_dl_bm25f
TESTING_REGRESS += bm25/fuzzy_seek
TESTING_REGRESS += bm25/include/filter
TESTING_REGRESS += bm25/include/store
TESTING_REGRESS += bm25/index/build_postings
TESTING_REGRESS += bm25/index/build_thresholds
TESTING_REGRESS += bm25/index/parts
TESTING_REGRESS += bm25/kernels/%
TESTING_REGRESS += bm25/lang_chain
TESTING_REGRESS += bm25/lang_memo
TESTING_REGRESS += bm25/lazy_tail_maxscore
TESTING_REGRESS += bm25/merge/heal
TESTING_REGRESS += bm25/merge/parts
TESTING_REGRESS += bm25/par_part_ranked
TESTING_REGRESS += bm25/position_blocks
TESTING_REGRESS += bm25/read/%
TESTING_REGRESS += bm25/store/%
TESTING_REGRESS += bm25/store/sweep_runs
TESTING_REGRESS += bm25/tidbridge
TESTING_REGRESS += bm25/verify/%
TESTING_REGRESS += bm25/wbuf/bind_mem
TESTING_REGRESS += bm25/wbuf_read_promote
TESTING_REGRESS += bm25/wbuf_wide_bloom
TESTING_REGRESS += col/advice_connect
TESTING_REGRESS += col/advice_connect_keycut
TESTING_REGRESS += col/agg/f1_decode_price
TESTING_REGRESS += col/alloc_limit
TESTING_REGRESS += col/alloc_scaled
TESTING_REGRESS += col/alter_layout
TESTING_REGRESS += col/alter_options
TESTING_REGRESS += col/approx
TESTING_REGRESS += col/auto_merge/basic
TESTING_REGRESS += col/auto_merge/partition
TESTING_REGRESS += col/batch_bits_bounds
TESTING_REGRESS += col/build/choose
TESTING_REGRESS += col/build/memory
TESTING_REGRESS += col/build/merge_ends
TESTING_REGRESS += col/build/merge_partitions
TESTING_REGRESS += col/build/open
TESTING_REGRESS += col/build/par_choice
TESTING_REGRESS += col/build/par_rows
TESTING_REGRESS += col/build/paths
TESTING_REGRESS += col/build/same_options
TESTING_REGRESS += col/build/spool
TESTING_REGRESS += col/build/spool_values
TESTING_REGRESS += col/calendar_columns
TESTING_REGRESS += col/census_sample
TESTING_REGRESS += col/check
TESTING_REGRESS += col/codec/%
TESTING_REGRESS += col/conn/auto
TESTING_REGRESS += col/conn/declare
TESTING_REGRESS += col/conn/fill
TESTING_REGRESS += col/conn/found
TESTING_REGRESS += col/conn/inserted
TESTING_REGRESS += col/conn/invert
TESTING_REGRESS += col/conn/journal
TESTING_REGRESS += col/conn/journal_mem
TESTING_REGRESS += col/conn/keyset
TESTING_REGRESS += col/conn/probe
TESTING_REGRESS += col/conn/refresh
TESTING_REGRESS += col/conn/reindex
TESTING_REGRESS += col/conn/relocate
TESTING_REGRESS += col/cube_priority
TESTING_REGRESS += col/cube_rank
TESTING_REGRESS += col/cut_aligned
TESTING_REGRESS += col/damage
TESTING_REGRESS += col/dead_keys_budget
TESTING_REGRESS += col/dead_rewrite
TESTING_REGRESS += col/delta_scoped
TESTING_REGRESS += col/delta_unchanged
TESTING_REGRESS += col/delvec_delta_chunks
TESTING_REGRESS += col/dict/bisect
TESTING_REGRESS += col/dict/blocks_hash
TESTING_REGRESS += col/dict/cache
TESTING_REGRESS += col/dict/count
TESTING_REGRESS += col/dict/hash
TESTING_REGRESS += col/dict/lookup
TESTING_REGRESS += col/dict/packed
TESTING_REGRESS += col/dict/shared
TESTING_REGRESS += col/dict/shared_stale
TESTING_REGRESS += col/dict/stored
TESTING_REGRESS += col/dir_lineage
TESTING_REGRESS += col/dir_nested
TESTING_REGRESS += col/dir_past_policy
TESTING_REGRESS += col/document_present
TESTING_REGRESS += col/dsm/write_caches
TESTING_REGRESS += col/dsm/write_statements
TESTING_REGRESS += col/dv/key_range
TESTING_REGRESS += col/dv/memory
TESTING_REGRESS += col/dv/oracle
TESTING_REGRESS += col/dv/reads_paid
TESTING_REGRESS += col/dv/switch
TESTING_REGRESS += col/enc
TESTING_REGRESS += col/f2/2d_prune_aio
TESTING_REGRESS += col/f2/agg_live
TESTING_REGRESS += col/f2/am
TESTING_REGRESS += col/f2/arrays
TESTING_REGRESS += col/f2/cache_lifecycle
TESTING_REGRESS += col/f2/check_blockmap
TESTING_REGRESS += col/f2/check_heap
TESTING_REGRESS += col/f2/check_heapallindexed
TESTING_REGRESS += col/f2/compact
TESTING_REGRESS += col/f2/cube/partial_twohop
TESTING_REGRESS += col/f2/cube/refresh_race
TESTING_REGRESS += col/f2/cube/router_crossover
TESTING_REGRESS += col/f2/ddl_catalog_sync
TESTING_REGRESS += col/f2/decoders
TESTING_REGRESS += col/f2/deep_compress
TESTING_REGRESS += col/f2/default_nan_scan
TESTING_REGRESS += col/f2/delta_chain
TESTING_REGRESS += col/f2/delta_unnest_pull
TESTING_REGRESS += col/f2/dv_delta_chunks
TESTING_REGRESS += col/f2/ext_safety
TESTING_REGRESS += col/f2/float_zones
TESTING_REGRESS += col/f2/flush_slab_turnover
TESTING_REGRESS += col/f2/framed_chunks
TESTING_REGRESS += col/f2/fsm
TESTING_REGRESS += col/f2/heapref_l0_wal
TESTING_REGRESS += col/f2/index_format_compat
TESTING_REGRESS += col/f2/index_stats
TESTING_REGRESS += col/f2/inspect_sql
TESTING_REGRESS += col/f2/inspect_tiered
TESTING_REGRESS += col/f2/inval_truncate
TESTING_REGRESS += col/f2/journal_ddl
TESTING_REGRESS += col/f2/json_leaf_tail_batch
TESTING_REGRESS += col/f2/key_order
TESTING_REGRESS += col/f2/limits
TESTING_REGRESS += col/f2/limits_ios
TESTING_REGRESS += col/f2/marks_prewhere
TESTING_REGRESS += col/f2/mergejoin_mark
TESTING_REGRESS += col/f2/meta_marks_suite
TESTING_REGRESS += col/f2/mutation
TESTING_REGRESS += col/f2/mvcc_churn
TESTING_REGRESS += col/f2/on63c_parity
TESTING_REGRESS += col/f2/ordered_scan
TESTING_REGRESS += col/f2/pack_one_commit
TESTING_REGRESS += col/f2/parallel_conc
TESTING_REGRESS += col/f2/partitioned_agg
TESTING_REGRESS += col/f2/partitioned_only
TESTING_REGRESS += col/f2/point_v2
TESTING_REGRESS += col/f2/preagg_table_par
TESTING_REGRESS += col/f2/prepare_inval
TESTING_REGRESS += col/f2/proj_ddl_acid
TESTING_REGRESS += col/f2/proj_rebuild
TESTING_REGRESS += col/f2/proj_tiered
TESTING_REGRESS += col/f2/projections
TESTING_REGRESS += col/f2/prune
TESTING_REGRESS += col/f2/pull_prune
TESTING_REGRESS += col/f2/quarantine
TESTING_REGRESS += col/f2/relcache_scan_delta
TESTING_REGRESS += col/f2/relcache_uaf_closure
TESTING_REGRESS += col/f2/reloptions_deprecated
TESTING_REGRESS += col/f2/scope_ddl
TESTING_REGRESS += col/f2/scope_live
TESTING_REGRESS += col/f2/size
TESTING_REGRESS += col/f2/stage_spec_abort
TESTING_REGRESS += col/f2/stage_subxact
TESTING_REGRESS += col/f2/status_heapref_slab
TESTING_REGRESS += col/f2/stor_mem
TESTING_REGRESS += col/f2/storage_codec_suite
TESTING_REGRESS += col/f2/storage_swap_staged
TESTING_REGRESS += col/f2/tier_drop_remote
TESTING_REGRESS += col/f2/tier_orphans
TESTING_REGRESS += col/f2/tier_policy_cache
TESTING_REGRESS += col/f2/tier_privileges
TESTING_REGRESS += col/f2/tier_wide
TESTING_REGRESS += col/f2/tiering
TESTING_REGRESS += col/f2/tiering_ttl
TESTING_REGRESS += col/f2/time_dirty_window
TESTING_REGRESS += col/f2/topn_tied_first_key
TESTING_REGRESS += col/f2/ux_progress
TESTING_REGRESS += col/f2/vacuum_horizon
TESTING_REGRESS += col/f2/wide_part
TESTING_REGRESS += col/f2/zero_unsupported
TESTING_REGRESS += col/f2/zero_unsupported_nan
TESTING_REGRESS += col/fold_due_count
TESTING_REGRESS += col/fold_keys_mem
TESTING_REGRESS += col/fold_marks
TESTING_REGRESS += col/format4
TESTING_REGRESS += col/format5
TESTING_REGRESS += col/gorder/%
TESTING_REGRESS += col/heapfree/pending
TESTING_REGRESS += col/heapfree/stamp_words
TESTING_REGRESS += col/heapfree/xmin
TESTING_REGRESS += col/holes_fuzz
TESTING_REGRESS += col/index/%
TESTING_REGRESS += col/insert_context_lazy
TESTING_REGRESS += col/ioss_columns
TESTING_REGRESS += col/json_null_scan
TESTING_REGRESS += col/kernels_simd%
TESTING_REGRESS += col/key/columns_auto
TESTING_REGRESS += col/key/presence
TESTING_REGRESS += col/key/runs
TESTING_REGRESS += col/key/table
TESTING_REGRESS += col/key_cut/basic
TESTING_REGRESS += col/key_cut/big_overlays
TESTING_REGRESS += col/key_cut/btree
TESTING_REGRESS += col/key_cut/grown_overlay
TESTING_REGRESS += col/key_cut/overlap
TESTING_REGRESS += col/key_cut/overlay_half
TESTING_REGRESS += col/key_cut/overlay_mass
TESTING_REGRESS += col/key_cut/overlay_pieces
TESTING_REGRESS += col/key_cut/paced
TESTING_REGRESS += col/key_cut/partition
TESTING_REGRESS += col/key_cut/ranges
TESTING_REGRESS += col/key_cut/ranges_small
TESTING_REGRESS += col/key_cut/segment_cap
TESTING_REGRESS += col/key_cut/switch_spill
TESTING_REGRESS += col/key_cut/unrecorded
TESTING_REGRESS += col/key_cut/workers
TESTING_REGRESS += col/keystore_prepared_drop
TESTING_REGRESS += col/leaf_fold
TESTING_REGRESS += col/leaf_stats
TESTING_REGRESS += col/list_test_once
TESTING_REGRESS += col/live_repair
TESTING_REGRESS += col/log_newer_format
TESTING_REGRESS += col/merge/dict_stream
TESTING_REGRESS += col/merge/growth
TESTING_REGRESS += col/merge/idempotent
TESTING_REGRESS += col/merge/key_cut_minimum
TESTING_REGRESS += col/merge/keys
TESTING_REGRESS += col/merge/overlay_runs
TESTING_REGRESS += col/merge/partition_setting
TESTING_REGRESS += col/merge/settled
TESTING_REGRESS += col/option_names_bytes
TESTING_REGRESS += col/order/%
TESTING_REGRESS += col/order_layout
TESTING_REGRESS += col/overlay/cap
TESTING_REGRESS += col/overlay/merge
TESTING_REGRESS += col/overlay/unordered
TESTING_REGRESS += col/partition/adopt
TESTING_REGRESS += col/parts_dead_delta
TESTING_REGRESS += col/paths_auto
TESTING_REGRESS += col/paths_nested
TESTING_REGRESS += col/peers_dead_scan
TESTING_REGRESS += col/point/%
TESTING_REGRESS += col/pre_census_lazy
TESTING_REGRESS += col/preagg/%
TESTING_REGRESS += col/proj/analytics_parity
TESTING_REGRESS += col/proj/live_churn
TESTING_REGRESS += col/proj/tail_reclaim
TESTING_REGRESS += col/range_count
TESTING_REGRESS += col/range_count_dirty
TESTING_REGRESS += col/range_order
TESTING_REGRESS += col/read_ahead
TESTING_REGRESS += col/refresh
TESTING_REGRESS += col/reindex/from_index
TESTING_REGRESS += col/reindex/heap_reads
TESTING_REGRESS += col/relcache_inval
TESTING_REGRESS += col/rewrite_cut
TESTING_REGRESS += col/rewrite_roll
TESTING_REGRESS += col/rollup_auto
TESTING_REGRESS += col/scan/key_range
TESTING_REGRESS += col/scan/unit_order
TESTING_REGRESS += col/scope/alter
TESTING_REGRESS += col/scope/auto_rollup
TESTING_REGRESS += col/scope/build
TESTING_REGRESS += col/scope/build_rule
TESTING_REGRESS += col/scope/build_temp
TESTING_REGRESS += col/scope/changed_pairs
TESTING_REGRESS += col/scope/cols
TESTING_REGRESS += col/scope/cols_maint
TESTING_REGRESS += col/scope/covered
TESTING_REGRESS += col/scope/ddl_journal_suite
TESTING_REGRESS += col/scope/dead_stamp
TESTING_REGRESS += col/scope/declare
TESTING_REGRESS += col/scope/dedupe
TESTING_REGRESS += col/scope/delta
TESTING_REGRESS += col/scope/groupkey
TESTING_REGRESS += col/scope/groups
TESTING_REGRESS += col/scope/hot_chain
TESTING_REGRESS += col/scope/journal/auto
TESTING_REGRESS += col/scope/journal/basic
TESTING_REGRESS += col/scope/journal/format
TESTING_REGRESS += col/scope/journal/tags
TESTING_REGRESS += col/scope/keyed_patch
TESTING_REGRESS += col/scope/maint
TESTING_REGRESS += col/scope/manymany
TESTING_REGRESS += col/scope/manymany_edit
TESTING_REGRESS += col/scope/member_lookup
TESTING_REGRESS += col/scope/merge_end
TESTING_REGRESS += col/scope/multiplicity
TESTING_REGRESS += col/scope/notice_minor
TESTING_REGRESS += col/scope/own_command
TESTING_REGRESS += col/scope/pairs/multiset
TESTING_REGRESS += col/scope/pairs/multiset_fill
TESTING_REGRESS += col/scope/pairs/multiset_partial
TESTING_REGRESS += col/scope/pairs/multiset_twohop
TESTING_REGRESS += col/scope/pairs/partial
TESTING_REGRESS += col/scope/pairs/recorded
TESTING_REGRESS += col/scope/pairs_spill
TESTING_REGRESS += col/scope/par_estimate
TESTING_REGRESS += col/scope/parallel
TESTING_REGRESS += col/scope/patch/basic
TESTING_REGRESS += col/scope/patch/members
TESTING_REGRESS += col/scope/patch/price
TESTING_REGRESS += col/scope/precheck
TESTING_REGRESS += col/scope/preconditions
TESTING_REGRESS += col/scope/price_terms
TESTING_REGRESS += col/scope/privileges
TESTING_REGRESS += col/scope/read
TESTING_REGRESS += col/scope/refresh_price
TESTING_REGRESS += col/scope/reindex_btree
TESTING_REGRESS += col/scope/roots_vacuum
TESTING_REGRESS += col/scope/sets
TESTING_REGRESS += col/scope/stamp
TESTING_REGRESS += col/scope/summaries
TESTING_REGRESS += col/scope/text/key
TESTING_REGRESS += col/scope/text/key_refusals
TESTING_REGRESS += col/scope/text/key_tags
TESTING_REGRESS += col/scope/text/member
TESTING_REGRESS += col/scope/text/member_collide
TESTING_REGRESS += col/scope/text/twohop
TESTING_REGRESS += col/scope/text/twohop_collide
TESTING_REGRESS += col/scope/twohop/build
TESTING_REGRESS += col/scope/twohop/choice
TESTING_REGRESS += col/scope/twohop/declare
TESTING_REGRESS += col/scope/twohop/journal
TESTING_REGRESS += col/scope/twohop/patch
TESTING_REGRESS += col/scope/twohop/tags
TESTING_REGRESS += col/scope/verify
TESTING_REGRESS += col/scope/verify_order
TESTING_REGRESS += col/scope/workers
TESTING_REGRESS += col/scope/zero_entry
TESTING_REGRESS += col/seg_head
TESTING_REGRESS += col/seg_mirror
TESTING_REGRESS += col/segfmt_mixed%
TESTING_REGRESS += col/set_list_span
TESTING_REGRESS += col/set_pres_budget
TESTING_REGRESS += col/set_summary
TESTING_REGRESS += col/shred/%
TESTING_REGRESS += col/skip_index_option
TESTING_REGRESS += col/slice_aligned
TESTING_REGRESS += col/stage_expr_fn
TESTING_REGRESS += col/stmt_cid
TESTING_REGRESS += col/stmt_cid_bm25
TESTING_REGRESS += col/subplan_probe
TESTING_REGRESS += col/suggest_paths
TESTING_REGRESS += col/supersede_source
TESTING_REGRESS += col/tags
TESTING_REGRESS += col/text/contains
TESTING_REGRESS += col/text/ilike
TESTING_REGRESS += col/text/patterns
TESTING_REGRESS += col/text/scanwrap
TESTING_REGRESS += col/text/tsig
TESTING_REGRESS += col/tomb/horizon
TESTING_REGRESS += col/tomb/log
TESTING_REGRESS += col/tomb/log_block_cache
TESTING_REGRESS += col/tomb/log_cache
TESTING_REGRESS += col/tomb/stale_header
TESTING_REGRESS += col/tomb/sync_mem
TESTING_REGRESS += col/topn/arith
TESTING_REGRESS += col/topn/case
TESTING_REGRESS += col/topn/code
TESTING_REGRESS += col/topn/deep
TESTING_REGRESS += col/topn/deep_limits
TESTING_REGRESS += col/topn/enum
TESTING_REGRESS += col/topn/expr
TESTING_REGRESS += col/topn/func
TESTING_REGRESS += col/topn/gorder_filter_safe
TESTING_REGRESS += col/topn/gorder_suite
TESTING_REGRESS += col/topn/interval
TESTING_REGRESS += col/topn/join
TESTING_REGRESS += col/topn/minmax_sublink
TESTING_REGRESS += col/topn/numeric
TESTING_REGRESS += col/topn/offset
TESTING_REGRESS += col/topn/or_arms
TESTING_REGRESS += col/topn/raw
TESTING_REGRESS += col/topn/rescan_keep
TESTING_REGRESS += col/topn/text
TESTING_REGRESS += col/topn/ties
TESTING_REGRESS += col/topn/trail
TESTING_REGRESS += col/topn/unit_ends
TESTING_REGRESS += col/topn/walk_cache
TESTING_REGRESS += col/track/ctid
TESTING_REGRESS += col/track/ctid_chains
TESTING_REGRESS += col/track/default
TESTING_REGRESS += col/track/deletes
TESTING_REGRESS += col/track/drop_concurrently
TESTING_REGRESS += col/track/key_quoted
TESTING_REGRESS += col/track/own_statement_reads
TESTING_REGRESS += col/track/rebaseline
TESTING_REGRESS += col/track/trigger_names
TESTING_REGRESS += col/track/update_skip
TESTING_REGRESS += col/track/update_visibility
TESTING_REGRESS += col/track/updates_supersede
TESTING_REGRESS += col/unique_stage
TESTING_REGRESS += col/unit_floor
TESTING_REGRESS += col/unit_of_block
TESTING_REGRESS += col/v1/star_part_preserved
TESTING_REGRESS += col/vacuum/parallel_keys
TESTING_REGRESS += col/vet_lazy_offsets
TESTING_REGRESS += col/vis_count
TESTING_REGRESS += col/volatile_notice
TESTING_REGRESS += col/wbuf/arena/basic
TESTING_REGRESS += col/wbuf/arena/conn
TESTING_REGRESS += col/wbuf/arena/dbs
TESTING_REGRESS += col/wbuf/arena/format
TESTING_REGRESS += col/wbuf/arena/gc
TESTING_REGRESS += col/wbuf/arena/relocate
TESTING_REGRESS += col/wbuf/arena/shrink
TESTING_REGRESS += col/wbuf/basic
TESTING_REGRESS += col/wbuf/big_set
TESTING_REGRESS += col/wbuf/cache
TESTING_REGRESS += col/wbuf/keyrange
TESTING_REGRESS += col/wbuf/lookup
TESTING_REGRESS += col/wbuf/order_cache
TESTING_REGRESS += col/wbuf/read_priced
TESTING_REGRESS += col/wbuf/reader_memory
TESTING_REGRESS += col/wbuf/reads_write_path
TESTING_REGRESS += col/wbuf/ref_conn
TESTING_REGRESS += col/wide_values
TESTING_REGRESS += col/wildcard_keys
TESTING_REGRESS += col/worker/failure
TESTING_REGRESS += col/writer_spool_mem
TESTING_REGRESS += col/writer_terms_mem
TESTING_REGRESS += col/zone/jnull
TESTING_REGRESS += compat/format_v1_v2
TESTING_REGRESS += compat/v1/scan_cache
TESTING_REGRESS += compat/v1/scan_dict_memory
TESTING_REGRESS += compat/v1/table_rewrite
TESTING_REGRESS += compat/v1/tomb_work_mem
TESTING_REGRESS += compat/v1/track_reindex
TESTING_REGRESS += compat/v1/unified_datum_fold
TESTING_REGRESS += compat/v1/unified_gorder_cursor
TESTING_REGRESS += compat/v1/unified_jsonb_leaf
TESTING_REGRESS += compat/v1/unified_jsonb_member
TESTING_REGRESS += compat/v1/vacuum_cap
TESTING_REGRESS += core/cache_budget
TESTING_REGRESS += core/cache_discard_all
TESTING_REGRESS += core/cache_eviction_safety
TESTING_REGRESS += core/errors_hints
TESTING_REGRESS += core/ext_recreate
TESTING_REGRESS += core/extlock_errors
TESTING_REGRESS += core/leak_planted
TESTING_REGRESS += core/limits_registry
TESTING_REGRESS += core/par/pool
TESTING_REGRESS += core/par/pool_limits
TESTING_REGRESS += core/radix_sort
TESTING_REGRESS += core/range
TESTING_REGRESS += core/ring_wrap
TESTING_REGRESS += core/scaling_ops_gate
TESTING_REGRESS += core/shm_ledger
TESTING_REGRESS += core/shm_manifest
TESTING_REGRESS += core/shm_vm_uniq_%
TESTING_REGRESS += core/spool
TESTING_REGRESS += core/sql_security
TESTING_REGRESS += core/surface_null_args
TESTING_REGRESS += core/tidbridge
TESTING_REGRESS += maint/%
TESTING_REGRESS += quarantine/bm25
TESTING_REGRESS += quarantine/build
TESTING_REGRESS += quarantine/capacity
TESTING_REGRESS += quarantine/col
TESTING_REGRESS += quarantine/damage
TESTING_REGRESS += quarantine/errors
TESTING_REGRESS += quarantine/hints
TESTING_REGRESS += quarantine/inject
TESTING_REGRESS += quarantine/join
TESTING_REGRESS += quarantine/parallel
TESTING_REGRESS += quarantine/partitions
TESTING_REGRESS += quarantine/persist
TESTING_REGRESS += quarantine/random
TESTING_REGRESS += quarantine/silent
TESTING_REGRESS += quarantine/track
TESTING_REGRESS += quarantine/txstate
TESTING_REGRESS += table/attach_detach
TESTING_REGRESS += table/bigtx
TESTING_REGRESS += table/bitmap_lazy
TESTING_REGRESS += table/bulk/eligible
TESTING_REGRESS += table/dead
TESTING_REGRESS += table/detoast_insert_memory
TESTING_REGRESS += table/empty_format1
TESTING_REGRESS += table/f2/autottl_rekey
TESTING_REGRESS += table/f2/col_delta
TESTING_REGRESS += table/f2/col_delta_dir
TESTING_REGRESS += table/f2/col_delta_hot
TESTING_REGRESS += table/f2/col_delta_summ
TESTING_REGRESS += table/f2/col_delta_trigger
TESTING_REGRESS += table/f2/copy_schema_evol
TESTING_REGRESS += table/f2/dml_cache_budget
TESTING_REGRESS += table/f2/drop_in_txn
TESTING_REGRESS += table/f2/drop_parts_before
TESTING_REGRESS += table/f2/fast_fetch
TESTING_REGRESS += table/f2/fetch
TESTING_REGRESS += table/f2/fetch_dv
TESTING_REGRESS += table/f2/join_parts
TESTING_REGRESS += table/f2/key_order_check
TESTING_REGRESS += table/f2/lock_escalation
TESTING_REGRESS += table/f2/native_strings_legacy
TESTING_REGRESS += table/f2/owner_sql_search_path
TESTING_REGRESS += table/f2/parity
TESTING_REGRESS += table/f2/pgstat
TESTING_REGRESS += table/f2/plans
TESTING_REGRESS += table/f2/row_pack
TESTING_REGRESS += table/f2/scan_pacer
TESTING_REGRESS += table/f2/temp_shm
TESTING_REGRESS += table/f2/tid_range
TESTING_REGRESS += table/f2/ttl_drop_acid
TESTING_REGRESS += table/f2/unified_parity
TESTING_REGRESS += table/f2/update_indexed
TESTING_REGRESS += table/fast_paths
TESTING_REGRESS += table/fetch_dict
TESTING_REGRESS += table/filter
TESTING_REGRESS += table/hot_sift_index
TESTING_REGRESS += table/key
TESTING_REGRESS += table/limits
TESTING_REGRESS += table/limits_lowered
TESTING_REGRESS += table/options
TESTING_REGRESS += table/page_corrupt
TESTING_REGRESS += table/preagg/calendar
TESTING_REGRESS += table/promote
TESTING_REGRESS += table/tomb/legacy_header
TESTING_REGRESS += table/tomb/log_limit
TESTING_REGRESS += table/v1/vacuum_truncate
TESTING_REGRESS += table/vacuum/batches
TESTING_REGRESS += table/vacuum/page_rows
TESTING_REGRESS += table/vacuum/unblock
TESTING_REGRESS += table/wide_dict_limit
TESTING_REGRESS += table/wide_rows
TESTING_REGRESS += table/workers
TESTING_REGRESS += table/writepath
# these assert the segments of a parallel build of an index with a key cut, which the release library builds in one process (build_pays in src/col/v1/build_par.c)
TESTING_REGRESS += agg/numeric/in_list
TESTING_REGRESS += agg/shape_widths
TESTING_REGRESS += col/cost_keys_sorted_once
TESTING_REGRESS += col/cost_selectivity_once
# Listed although scripts/check-testing-regress.sh would not select them (each fails on the release library, so the stock-library jobs must not run it):
# sift.test_run_docs, a setting only the testing build has
TESTING_REGRESS += bm25/build/memory
TESTING_REGRESS += bm25/build/parallel
TESTING_REGRESS += bm25/build/runs
# sift.test_par_workers, a setting only the testing build has
TESTING_REGRESS += bm25/parallel/build
TESTING_REGRESS += table/bulk/parallel
# sift.test_range_pieces and sift.test_range_bytes, settings only the testing build has
TESTING_REGRESS += bm25/build/ranges
TESTING_REGRESS += bm25/merge/ranges
# sift.test_page_text, a setting only the testing build has
TESTING_REGRESS += bm25/build/large_docs
# sift.grouped_min_df, a setting only the testing build has
TESTING_REGRESS += bm25/list_slice_seek
TESTING_REGRESS += bm25/rank/or_union
TESTING_REGRESS += bm25/rank/page_live
# sift.test_rank_mode, a setting only the testing build has
TESTING_REGRESS += bm25/rank/modes
# sift.table_max_segments and sift.table_vblock_floor, settings only the testing build has
TESTING_REGRESS += table/bulk/groups
TESTING_REGRESS += table/cap
TESTING_REGRESS += table/vblocks
TESTING_REGRESS += table/verify_frozen
# an asserted value differs on the release library
TESTING_REGRESS += agg/count_not_null
TESTING_REGRESS += agg/preagg/distinct
TESTING_REGRESS += agg/preagg/linear
TESTING_REGRESS += agg/privileges
TESTING_REGRESS += agg/refusals
TESTING_REGRESS += agg/set_array
TESTING_REGRESS += agg/unstorable_values
TESTING_REGRESS += agg/window/basic
TESTING_REGRESS += bm25/facet_counts
TESTING_REGRESS += col/build/open_memory
TESTING_REGRESS += col/build/peers
TESTING_REGRESS += col/build/peers_unequal
TESTING_REGRESS += col/build/spool_memory
TESTING_REGRESS += col/conn/reads
TESTING_REGRESS += col/merge/memory
TESTING_REGRESS += col/scan/basic
TESTING_REGRESS += core/cache_discard
TESTING_REGRESS += core/roles/ddl
TESTING_REGRESS += core/roles/dml_query
TESTING_REGRESS += core/roles/maintenance
TESTING_REGRESS += table/agg/basic
# sift._test_s55_* functions, only the testing build has
TESTING_REGRESS += col/f2/point_dir
TESTING_REGRESS += col/granule/point
TESTING_REGRESS += col/key/order
# the tests of the Format 1 settings that sift.test_* settings replaced (the testing build has them, the release library has not)
TESTING_REGRESS += agg/cte/inline_band
TESTING_REGRESS += agg/cte/inline_rows
TESTING_REGRESS += agg/lookup/basic
TESTING_REGRESS += agg/lookup/keyed_crossover
TESTING_REGRESS += agg/preagg/elem_parallel
TESTING_REGRESS += agg/preagg/exceptions
TESTING_REGRESS += agg/preagg/super
TESTING_REGRESS += agg/stats
TESTING_REGRESS += col/key_cut/advice
TESTING_REGRESS += col/key_cut/overlays_suite
TESTING_REGRESS += col/key_cut/point
TESTING_REGRESS += col/key_cut/search
TESTING_REGRESS += col/key_cut/split
TESTING_REGRESS += col/key_cut/switched
TESTING_REGRESS += col/key_cut/units
TESTING_REGRESS += col/merge/force
TESTING_REGRESS += col/merge/levels
TESTING_REGRESS += col/merge/runs
TESTING_REGRESS += col/overlay/lean
TESTING_REGRESS += col/overlay/wide_units
TESTING_REGRESS += col/search
TESTING_REGRESS += col/sections
TESTING_REGRESS += col/settings_surface
TESTING_REGRESS += col/topn/basic
TESTING_REGRESS += col/topn/order
TESTING_REGRESS += col/topn/parallel
TESTING_REGRESS += col/track/attach
TESTING_REGRESS += col/track/old_extension
TESTING_REGRESS += col/track/partitions
TESTING_REGRESS += col/track/pending
TESTING_REGRESS += col/track/reads
TESTING_REGRESS += col/track/update_window
TESTING_REGRESS += col/track/updates_option
TESTING_REGRESS += col/wbuf/batch
TESTING_REGRESS += col/wbuf/promote_stream
TESTING_REGRESS += col/wbuf/ref_hot
TESTING_REGRESS += col/wbuf/ref_prune
TESTING_REGRESS += col/wbuf/scan_seal_suite
TESTING_REGRESS += col/wbuf/slots
TESTING_REGRESS += compat/v1/store_ops
TESTING_REGRESS += compat/v1/store_ops_ext
TESTING_REGRESS += compat/v1/store_ops_par_zone
TESTING_REGRESS += compat/v1/unified_jsonb_typeof
TESTING_REGRESS += compat/v1/unified_preagg_cube
TESTING_REGRESS += compat/v1/unified_rows_scankey
TESTING_REGRESS += compat/v1/unified_scan
TESTING_REGRESS += compat/v1/unified_star_keys
TESTING_REGRESS += compat/v1/unified_text_rows
TESTING_REGRESS += compat/v1/unified_topn_rows
TESTING_REGRESS += table/lists_runs
TESTING_REGRESS += table/preagg/minmax
TESTING_REGRESS += table/topn
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

# SIFT_HARDEN=pkg|init|1: opt-in compiler hardening (scripts/harden.mk, docs/SAFETY.md)
include $(dir $(SIFT_MK))scripts/harden.mk

# make check only runs scripts/ci.sh, which builds and installs on its own; PGXS would
# claim "check" is unsupported, so it is not included for these goals (SIFT_DEV_GOALS). pg-assert and pg-valgrind build the PostgreSQL of the `assert`
# and `valgrind` CI lanes once into a cache outside the repo (scripts/pg-assert.sh; PG_ASSERT_MAJOR=17 for the 17 branch).
SIFT_DEV_GOALS := check lint dev-check check-scaling-ops pg-assert pg-valgrind
ifeq ($(filter $(SIFT_DEV_GOALS),$(MAKECMDGOALS)),)
include $(PGXS)

# The general-purpose layers of the column codec (src/col/compress.c): LZ4 and zstd, each linked; a header or library that is not found stops the build. They are probed with the compiler
# PGXS names (CC is not known before it is included), and their flag goes to CPPFLAGS, which PGXS has built from PG_CPPFLAGS by now.
# SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 build without one: chunks never use it, and one written elsewhere that does fails to open with a clear error.
# pg_config's flags carry the include and library directories of a package manager (Homebrew keeps lz4 and zstd out of the compiler's default paths)
colz_flags := $(shell $(PG_CONFIG) --cppflags --ldflags)
define colz_probe
$(if $(SIFT_NO_$(1)),,$(if $(shell echo 'int main(void) { return 0; }' | $(CC) $(colz_flags) -include $(2).h -x c - -l$(2) -o /dev/null 2>/dev/null && echo ok),$(eval override CPPFLAGS += -DSIFT_HAVE_$(1))$(eval SHLIB_LINK += -l$(2)),$(error pg_sift: $(2) not found (install lib$(2) headers, or build with SIFT_NO_$(1)=1))))
endef
$(call colz_probe,LZ4,lz4)
$(call colz_probe,ZSTD,zstd)

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
# one recipe builds every generated script: a rule with several targets would run it once for each of them under make -jN (a grouped target needs GNU make 4.3)
.sql-parts.stamp: $(wildcard sql/[0-9]*/*.sql) scripts/sql-parts.sh
	scripts/sql-parts.sh build
	@touch $@
$(SIFT_UPDATE_SQL): .sql-parts.stamp
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
SIFT_CODE_ID = $(shell $(srcdir)/scripts/code-id.sh $(or $(srcdir),.) '$(filter -DSIFT_%,$(CPPFLAGS))')
$(SIFT_GEN)/core/code_id.h: FORCE
	@mkdir -p $(@D); echo '#define SIFT_CODE_ID "$(SIFT_CODE_ID)"' >$@.new; cmp -s $@.new $@ || mv $@.new $@; rm -f $@.new
$(SIFT_OBJ)$(SRC)/col/dsmreg.o: $(SIFT_GEN)/core/code_id.h
# the two files that include them look in the build directory first: a header another build left in src/core is never read
ifneq ($(SIFT_OUT),)
$(SIFT_OBJ)$(SRC)/core/handler.o $(SIFT_OBJ)$(SRC)/col/dsmreg.o: override CPPFLAGS := -I$(SIFT_GEN) $(CPPFLAGS)
endif
.PHONY: FORCE
FORCE:

# the suites beyond installcheck and the other developer targets (scripts/dev-targets.mk)
include $(dir $(SIFT_MK))scripts/dev-targets.mk
