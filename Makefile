EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
OBJS = $(patsubst %.c,%.o,$(wildcard $(SRC)/*/*.c $(SRC)/*/*/*.c))
# The update script of the release in progress is built from sql/<version>/NN-<context>.sql (scripts/sql-parts.sh): edit a part, never the generated file.
SIFT_UPDATE_SQL := $(shell $(dir $(SIFT_MK))scripts/sql-parts.sh targets)
DATA = $(sort $(wildcard sql/pg_sift--*.sql) $(SIFT_UPDATE_SQL))
PGFILEDESC = "pg_sift - Fast analytics for PostgreSQL: keep your queries, just add an index. Plus BM25 full-text search"

# New SQL tests are registered automatically; the test-only patterns below apply to them too.
REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
# -MMD records header dependencies (src/*/*.d, src/*/*/*.d) so a header edit rebuilds its users.
PG_CPPFLAGS = $(addprefix -I$(srcdir)/,$(SIFT_INCDIRS))
# an implicit declaration is an error: gcc 13 only warns, and a function taken to return int cuts a pointer to 32 bits (an include dropped from a file whose code is built only with assertions did that)
PG_CFLAGS = -O2 -g -ffp-contract=off -MMD -Wno-declaration-after-statement -Werror=implicit-function-declaration $(SIFT_BASE_ISA)
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
EXTRA_CLEAN = $(SIFT_UPDATE_SQL) src/*/*.d src/*/*/*.d .build-flavor test/results test/regression.diffs test/regression.out
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
TESTING_REGRESS += agg_child_warm
TESTING_REGRESS += agg_combo_cap
TESTING_REGRESS += agg_cte_verdict_cache
TESTING_REGRESS += agg_dict_blocks_lazy
TESTING_REGRESS += agg_distinct_narrow
TESTING_REGRESS += agg_float_step
TESTING_REGRESS += agg_gord_group_skip_limit
TESTING_REGRESS += agg_group_hash_probe
TESTING_REGRESS += agg_group_nospill_count
TESTING_REGRESS += agg_group_nospill_run
TESTING_REGRESS += agg_group_passes
TESTING_REGRESS += agg_group_single_key
TESTING_REGRESS += agg_group_spill
TESTING_REGRESS += agg_group_spill_limits
TESTING_REGRESS += agg_int_key_ids
TESTING_REGRESS += agg_jsonb_key_forms
TESTING_REGRESS += agg_jsonb_key_read
TESTING_REGRESS += agg_jsonb_not_exist
TESTING_REGRESS += agg_jsonb_typeof
TESTING_REGRESS += agg_jsonpath_kinds
TESTING_REGRESS += agg_key_cube_fallback
TESTING_REGRESS += agg_key_list_once
TESTING_REGRESS += agg_limit_refusals
TESTING_REGRESS += agg_lookup_chain
TESTING_REGRESS += agg_outer_walks_once
TESTING_REGRESS += agg_par_blob_chunks
TESTING_REGRESS += agg_par_join_split
TESTING_REGRESS += agg_par_partitions
TESTING_REGRESS += agg_par_split
TESTING_REGRESS += agg_partitions
TESTING_REGRESS += agg_partitions_many
TESTING_REGRESS += agg_plan_passes
TESTING_REGRESS += agg_planner_state_error
TESTING_REGRESS += agg_preagg_elem
TESTING_REGRESS += agg_preagg_minmax
TESTING_REGRESS += agg_preagg_rollup
TESTING_REGRESS += agg_probe_direct
TESTING_REGRESS += agg_probe_direct_columns
TESTING_REGRESS += agg_probe_direct_race
TESTING_REGRESS += agg_root_key_exists
TESTING_REGRESS += agg_rows_group_memory
TESTING_REGRESS += agg_scope_choice
TESTING_REGRESS += agg_scope_group_key
TESTING_REGRESS += agg_scope_param_keys
TESTING_REGRESS += agg_scope_preserved
TESTING_REGRESS += agg_scope_recognition
TESTING_REGRESS += agg_scope_twohop_recognition
TESTING_REGRESS += agg_scope_union_group
TESTING_REGRESS += agg_scope_virtual_leaf
TESTING_REGRESS += agg_topn_key_only
TESTING_REGRESS += agg_topn_offset_bulk
TESTING_REGRESS += agg_topn_offset_dirty
TESTING_REGRESS += agg_topn_offset_keyed
TESTING_REGRESS += agg_topn_offset_ties
TESTING_REGRESS += agg_topn_with_ties
TESTING_REGRESS += agg_vis_counts
TESTING_REGRESS += aggkern_%
TESTING_REGRESS += aggstub_%
TESTING_REGRESS += analyzer_version
TESTING_REGRESS += bm25_dead_windows
TESTING_REGRESS += bm25_wbuf_read_promote
TESTING_REGRESS += bm25_wbuf_wide_bloom
TESTING_REGRESS += build_limits
TESTING_REGRESS += build_page_text
TESTING_REGRESS += build_pool
TESTING_REGRESS += col_alloc_limit
TESTING_REGRESS += col_alloc_scaled
TESTING_REGRESS += col_alter_options
TESTING_REGRESS += col_approx
TESTING_REGRESS += col_auto_merge
TESTING_REGRESS += col_auto_merge_partition
TESTING_REGRESS += col_build_merge_ends
TESTING_REGRESS += col_build_par_choice
TESTING_REGRESS += col_build_par_rows
TESTING_REGRESS += col_build_same_options
TESTING_REGRESS += col_calendar_columns
TESTING_REGRESS += col_census_sample
TESTING_REGRESS += col_conn_auto
TESTING_REGRESS += col_conn_declare
TESTING_REGRESS += col_conn_fill
TESTING_REGRESS += col_conn_found
TESTING_REGRESS += col_conn_inserted
TESTING_REGRESS += col_conn_invert
TESTING_REGRESS += col_conn_journal
TESTING_REGRESS += col_conn_keyset
TESTING_REGRESS += col_conn_probe
TESTING_REGRESS += col_conn_refresh
TESTING_REGRESS += col_conn_reindex
TESTING_REGRESS += col_conn_relocate
TESTING_REGRESS += col_cube_priority
TESTING_REGRESS += col_cube_rank
TESTING_REGRESS += col_damage
TESTING_REGRESS += col_dead_delta
TESTING_REGRESS += col_dead_keys_budget
TESTING_REGRESS += col_dead_rewrite
TESTING_REGRESS += col_deprecated_options
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
TESTING_REGRESS += col_document_present
TESTING_REGRESS += col_dsm_write_caches
TESTING_REGRESS += col_dsm_write_statements
TESTING_REGRESS += col_enc
TESTING_REGRESS += col_format4
TESTING_REGRESS += col_format5
TESTING_REGRESS += col_gorder
TESTING_REGRESS += col_gorder_auto
TESTING_REGRESS += col_gorder_autogroup
TESTING_REGRESS += col_gorder_autogroup_kinds
TESTING_REGRESS += col_gorder_chunks
TESTING_REGRESS += col_gorder_count_big_group
TESTING_REGRESS += col_gorder_dead_cache
TESTING_REGRESS += col_gorder_ends
TESTING_REGRESS += col_gorder_entity_group
TESTING_REGRESS += col_gorder_free_cache
TESTING_REGRESS += col_gorder_fullrange
TESTING_REGRESS += col_gorder_gaps
TESTING_REGRESS += col_gorder_group
TESTING_REGRESS += col_gorder_group_compact
TESTING_REGRESS += col_gorder_group_count
TESTING_REGRESS += col_gorder_group_packed
TESTING_REGRESS += col_gorder_group_points
TESTING_REGRESS += col_gorder_group_pos
TESTING_REGRESS += col_gorder_group_price
TESTING_REGRESS += col_gorder_group_probe_dead
TESTING_REGRESS += col_gorder_group_range
TESTING_REGRESS += col_gorder_group_walk
TESTING_REGRESS += col_gorder_heap_order_ends
TESTING_REGRESS += col_gorder_in_walk
TESTING_REGRESS += col_gorder_layout
TESTING_REGRESS += col_gorder_mem_segment
TESTING_REGRESS += col_gorder_narrow
TESTING_REGRESS += col_gorder_seek
TESTING_REGRESS += col_gorder_text
TESTING_REGRESS += col_gorder_text_buffer
TESTING_REGRESS += col_gorder_text_count
TESTING_REGRESS += col_gorder_textperm
TESTING_REGRESS += col_gorder_tiebreak
TESTING_REGRESS += col_gorder_track_scatter
TESTING_REGRESS += col_heapfree_xmin
TESTING_REGRESS += col_holes_fuzz
TESTING_REGRESS += col_index_%
TESTING_REGRESS += col_ioss_columns
TESTING_REGRESS += col_kernels_%
TESTING_REGRESS += col_key_columns_auto
TESTING_REGRESS += col_key_cut
TESTING_REGRESS += col_key_cut_big_overlays
TESTING_REGRESS += col_key_cut_grown_overlay
TESTING_REGRESS += col_key_cut_hot_value
TESTING_REGRESS += col_key_cut_overlap
TESTING_REGRESS += col_key_cut_overlay_mass
TESTING_REGRESS += col_key_cut_paced
TESTING_REGRESS += col_key_cut_partition
TESTING_REGRESS += col_key_cut_ranges
TESTING_REGRESS += col_key_cut_ranges_small
TESTING_REGRESS += col_key_cut_redecide
TESTING_REGRESS += col_key_cut_redecide_async
TESTING_REGRESS += col_key_cut_segment_cap
TESTING_REGRESS += col_key_presence
TESTING_REGRESS += col_key_runs
TESTING_REGRESS += col_key_table
TESTING_REGRESS += col_leaf_fold
TESTING_REGRESS += col_leaf_stats
TESTING_REGRESS += col_list_test_once
TESTING_REGRESS += col_log_newer_format
TESTING_REGRESS += col_merge_idempotent
TESTING_REGRESS += col_merge_keys
TESTING_REGRESS += col_minmax_runs
TESTING_REGRESS += col_net_policy
TESTING_REGRESS += col_order_layout
TESTING_REGRESS += col_ordered_budget
TESTING_REGRESS += col_overlay_unordered
TESTING_REGRESS += col_paths_auto
TESTING_REGRESS += col_peers_dead_scan
TESTING_REGRESS += col_point_array
TESTING_REGRESS += col_point_chunks
TESTING_REGRESS += col_point_dense
TESTING_REGRESS += col_point_dir
TESTING_REGRESS += col_point_exact
TESTING_REGRESS += col_point_lazy
TESTING_REGRESS += col_point_many_keys
TESTING_REGRESS += col_point_packed
TESTING_REGRESS += col_point_radix
TESTING_REGRESS += col_point_root
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
TESTING_REGRESS += col_rewrite_cut
TESTING_REGRESS += col_rewrite_roll
TESTING_REGRESS += col_rollup_auto
TESTING_REGRESS += col_rollup_refresh
TESTING_REGRESS += col_run_heads
TESTING_REGRESS += col_scan_unit_order
TESTING_REGRESS += col_scope_build
TESTING_REGRESS += col_scope_changed_pairs
TESTING_REGRESS += col_scope_cols
TESTING_REGRESS += col_scope_cols_maint
TESTING_REGRESS += col_scope_covered
TESTING_REGRESS += col_scope_declare
TESTING_REGRESS += col_scope_dedupe
TESTING_REGRESS += col_scope_delta
TESTING_REGRESS += col_scope_fill
TESTING_REGRESS += col_scope_groupkey
TESTING_REGRESS += col_scope_journal
TESTING_REGRESS += col_scope_journal_partial
TESTING_REGRESS += col_scope_keyed_patch
TESTING_REGRESS += col_scope_maint
TESTING_REGRESS += col_scope_manymany
TESTING_REGRESS += col_scope_manymany_edit
TESTING_REGRESS += col_scope_pairs_listed
TESTING_REGRESS += col_scope_pairs_multiset
TESTING_REGRESS += col_scope_pairs_multiset_fill
TESTING_REGRESS += col_scope_pairs_multiset_partial
TESTING_REGRESS += col_scope_pairs_multiset_twohop
TESTING_REGRESS += col_scope_pairs_partial
TESTING_REGRESS += col_scope_pairs_recorded
TESTING_REGRESS += col_scope_parallel
TESTING_REGRESS += col_scope_patch
TESTING_REGRESS += col_scope_patch_price
TESTING_REGRESS += col_scope_precheck
TESTING_REGRESS += col_scope_price_terms
TESTING_REGRESS += col_scope_read
TESTING_REGRESS += col_scope_refresh_price
TESTING_REGRESS += col_scope_twohop_build
TESTING_REGRESS += col_scope_twohop_declare
TESTING_REGRESS += col_scope_twohop_journal
TESTING_REGRESS += col_scope_twohop_patch
TESTING_REGRESS += col_scope_verify_order
TESTING_REGRESS += col_seg_head
TESTING_REGRESS += col_seg_mirror
TESTING_REGRESS += col_set_list_span
TESTING_REGRESS += col_set_summary
TESTING_REGRESS += col_sorted_run
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
TESTING_REGRESS += col_topn_arith
TESTING_REGRESS += col_topn_case
TESTING_REGRESS += col_topn_code
TESTING_REGRESS += col_topn_deep_limits
TESTING_REGRESS += col_topn_enum
TESTING_REGRESS += col_topn_expr
TESTING_REGRESS += col_topn_func
TESTING_REGRESS += col_topn_gorder
TESTING_REGRESS += col_topn_gorder_filter
TESTING_REGRESS += col_topn_gorder_filter_text
TESTING_REGRESS += col_topn_interval
TESTING_REGRESS += col_topn_numeric
TESTING_REGRESS += col_topn_offset
TESTING_REGRESS += col_topn_raw
TESTING_REGRESS += col_topn_text
TESTING_REGRESS += col_topn_text_offset
TESTING_REGRESS += col_topn_trail
TESTING_REGRESS += col_topn_unit_ends
TESTING_REGRESS += col_topn_walk_price
TESTING_REGRESS += col_track_ctid
TESTING_REGRESS += col_track_ctid_chains
TESTING_REGRESS += col_track_default
TESTING_REGRESS += col_track_deletes
TESTING_REGRESS += col_track_drop_concurrently
TESTING_REGRESS += col_track_own_statement_reads
TESTING_REGRESS += col_track_rebaseline
TESTING_REGRESS += col_track_reindex_concurrently
TESTING_REGRESS += col_track_shared_probe
TESTING_REGRESS += col_track_update_skip
TESTING_REGRESS += col_track_updates_supersede
TESTING_REGRESS += col_unit_floor
TESTING_REGRESS += col_unit_of_block
TESTING_REGRESS += col_vacuum_parallel_keys
TESTING_REGRESS += col_vet_lazy_offsets
TESTING_REGRESS += col_vis_count
TESTING_REGRESS += col_volatile_notice
TESTING_REGRESS += col_vstore_churn
TESTING_REGRESS += col_vstore_drops
TESTING_REGRESS += col_vstore_epochs
TESTING_REGRESS += col_vstore_garbage
TESTING_REGRESS += col_vstore_left
TESTING_REGRESS += col_vstore_lifecycle
TESTING_REGRESS += col_vstore_names
TESTING_REGRESS += col_vstore_runs
TESTING_REGRESS += col_vstore_segments
TESTING_REGRESS += col_wbuf
TESTING_REGRESS += col_wbuf_arena
TESTING_REGRESS += col_wbuf_arena_conn
TESTING_REGRESS += col_wbuf_arena_dbs
TESTING_REGRESS += col_wbuf_arena_format
TESTING_REGRESS += col_wbuf_arena_gc
TESTING_REGRESS += col_wbuf_arena_relocate
TESTING_REGRESS += col_wbuf_big_set
TESTING_REGRESS += col_wbuf_order_cache
TESTING_REGRESS += col_wbuf_read_priced
TESTING_REGRESS += col_wbuf_reads_write_path
TESTING_REGRESS += col_wbuf_ref_conn
TESTING_REGRESS += col_wide_values
TESTING_REGRESS += col_wildcard_keys
TESTING_REGRESS += col_worker_failure
TESTING_REGRESS += col_zone_jnull
TESTING_REGRESS += colcodec_%
TESTING_REGRESS += core_radix_sort
TESTING_REGRESS += errors_hints
TESTING_REGRESS += fuzzy_seek
TESTING_REGRESS += include_filter
TESTING_REGRESS += include_store
TESTING_REGRESS += index_build_postings
TESTING_REGRESS += index_build_thresholds
TESTING_REGRESS += index_parts
TESTING_REGRESS += kernels_%
TESTING_REGRESS += lang_chain
TESTING_REGRESS += lang_memo
TESTING_REGRESS += limits_registry
TESTING_REGRESS += maint_%
TESTING_REGRESS += merge_heal
TESTING_REGRESS += merge_parts
TESTING_REGRESS += position_blocks
TESTING_REGRESS += read_%
TESTING_REGRESS += segfmt_%
TESTING_REGRESS += shred_%
TESTING_REGRESS += slot_owner
TESTING_REGRESS += store_%
TESTING_REGRESS += store_sweep_runs
TESTING_REGRESS += tbl_bigtx
TESTING_REGRESS += tbl_bitmap_lazy
TESTING_REGRESS += tbl_bulk_eligible
TESTING_REGRESS += tbl_fetch_dict
TESTING_REGRESS += tbl_hot_sift_index
TESTING_REGRESS += tbl_key_table
TESTING_REGRESS += tbl_limits_lowered
TESTING_REGRESS += tbl_page_corrupt
TESTING_REGRESS += tbl_preagg_calendar
TESTING_REGRESS += tbl_tomb_legacy_header
TESTING_REGRESS += tbl_tomb_log_limit
TESTING_REGRESS += tbl_vacuum_batches
TESTING_REGRESS += tbl_wide_dict_limit
TESTING_REGRESS += tbl_wide_rows
TESTING_REGRESS += tbl_writepath
TESTING_REGRESS += verify_%
# these assert the segments of a parallel build of an index with a key cut, which the release library builds in one process (build_pays in src/col/build_par.c)
TESTING_REGRESS += agg_numeric_in_list
TESTING_REGRESS += agg_prepass_const_list
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
# sift.table_max_segments and sift.table_vblock_floor, settings only the testing build has
TESTING_REGRESS += tbl_bulk_groups
TESTING_REGRESS += tbl_cap
TESTING_REGRESS += tbl_vblocks
TESTING_REGRESS += tbl_verify_frozen
# an asserted value differs on the release library
TESTING_REGRESS += agg_preagg
TESTING_REGRESS += agg_preagg_rows
TESTING_REGRESS += agg_workers_few_units
TESTING_REGRESS += col_build_peers
TESTING_REGRESS += col_merge_memory
ifdef SIFT_SB_SHIFT
ifndef SIFT_TESTING
$(error SIFT_SB_SHIFT changes the on-disk geometry: for SIFT_TESTING=1 test flavours only)
endif
PG_CPPFLAGS += -DSIFT_SB_SHIFT=$(SIFT_SB_SHIFT)
endif
ifdef SIFT_TESTING
EXTENSION += pg_sift_test
DATA += sql/pg_sift_test--1.0.sql
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

# The general-purpose layers of the column codec (src/col/compress.c): LZ4 and zstd, each linked when its header and library are found.
# SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 build without one: chunks never use it, and one written elsewhere that does fails to open with a clear error.
# pg_config's flags carry the include and library directories of a package manager (Homebrew keeps lz4 and zstd out of the compiler's default paths)
colz_flags := $(shell $(PG_CONFIG) --cppflags --ldflags)
define colz_probe
$(if $(SIFT_NO_$(1)),,$(if $(shell echo 'int main(void) { return 0; }' | $(CC) $(colz_flags) -include $(2).h -x c - -l$(2) -o /dev/null 2>/dev/null && echo ok),$(eval PG_CPPFLAGS += -DSIFT_HAVE_$(1))$(eval SHLIB_LINK += -l$(2))))
endef
$(call colz_probe,LZ4,lz4)
$(call colz_probe,ZSTD,zstd)

# SIFT_HARDEN=pkg|init|1: opt-in compiler hardening (scripts/harden.mk, docs/SAFETY.md)
include $(dir $(SIFT_MK))scripts/harden.mk

# make check / check-full only run scripts/ci.sh, which builds and installs on its own; PGXS would
# claim "check" is unsupported, so it is not included for these goals. pg-assert and pg-valgrind build the PostgreSQL of the `assert`
# and `valgrind` CI lanes once into a cache outside the repo (scripts/pg-assert.sh; PG_ASSERT_MAJOR=17 for the 17 branch).
ifneq ($(filter check check-full pg-assert pg-valgrind,$(MAKECMDGOALS)),)
.PHONY: check check-full lint pg-assert pg-valgrind
check:
	scripts/ci.sh quick
check-full:
	scripts/ci.sh full
lint:
	scripts/lint.sh
pg-assert:
	nice scripts/pg-assert.sh $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
pg-valgrind:
	nice scripts/pg-assert.sh --valgrind $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
else
include $(PGXS)
endif

.PHONY: update-sql
$(SIFT_UPDATE_SQL): $(wildcard sql/[0-9]*/*.sql) scripts/sql-parts.sh
	scripts/sql-parts.sh build
update-sql: $(SIFT_UPDATE_SQL)
all: update-sql

-include $(wildcard src/*/*.d src/*/*/*.d)

# The module version the library reports (scripts/build-id.sh); build_id.h is rewritten only when the id changes, so that handler.o is rebuilt then and no other file.
SIFT_BUILD_ID ?= $(shell $(srcdir)/scripts/build-id.sh $(or $(srcdir),.))
$(SRC)/core/build_id.h: FORCE
	@echo '#define SIFT_BUILD_ID "$(SIFT_BUILD_ID)"' >$@.new; cmp -s $@.new $@ || mv $@.new $@; rm -f $@.new
$(SRC)/core/handler.o: $(SRC)/core/build_id.h
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
# docs/LIMITS.md from the registry in src/core/limits.h (scripts/gen-limits.sh --check proves it)
limits:
	scripts/gen-limits.sh

# Rebuilds src/agg/vk_spv.h from the shader (needs glslc) and the Vulkan declarations from /usr/include/vulkan; commit the result.
gpu-shaders:
	scripts/gen-aggvk-spv.sh
	scripts/gen-aggvk-min.py
