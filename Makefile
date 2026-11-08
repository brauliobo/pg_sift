EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
OBJS = $(patsubst %.c,%.o,$(wildcard $(SRC)/*/*.c $(SRC)/*/*/*.c))
DATA = $(wildcard sql/pg_sift--*.sql)
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
EXTRA_CLEAN = src/*/*.d src/*/*/*.d .build-flavor test/results test/regression.diffs test/regression.out
ifdef SANITIZE
PG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SHLIB_LINK += -fsanitize=address,undefined
endif

# SIFT_TESTING=1 compiles the sift._test_* functions and installs the
# test-only pg_sift_test extension; regress tests using it are skipped otherwise.
# Switching it needs `make clean`.
TESTING_REGRESS = shred_% index_parts segfmt_% col_index_% store_% kernels_% col_kernels_% aggkern_% col_enc col_wbuf colcodec_% verify_% analyzer_version index_build_postings index_build_thresholds read_% build_limits build_pool build_page_text aggstub_% maint_% tbl_bulk_eligible tbl_bigtx tbl_writepath tbl_page_corrupt col_format4 col_format5 col_tags agg_jsonb_typeof agg_jsonpath_kinds agg_probe_direct_columns
TESTING_REGRESS += core_radix_sort agg_group_hash_probe agg_int_key_ids tbl_bitmap_lazy limits_registry tbl_limits_lowered agg_combo_cap agg_limit_refusals agg_planner_state_error slot_owner
TESTING_REGRESS += col_log_newer_format col_heapfree_xmin col_tomb_stale_header col_tomb_horizon col_supersede_source col_track_update_skip include_store include_filter errors_hints agg_cte_verdict_cache col_leaf_stats col_minmax_runs agg_outer_walks_once fuzzy_seek agg_group_single_key
TESTING_REGRESS += col_track_ctid col_track_ctid_chains
TESTING_REGRESS += agg_key_list_once agg_plan_passes agg_vis_counts
TESTING_REGRESS += col_set_list_span agg_topn_with_ties agg_root_key_exists agg_jsonb_key_forms agg_jsonb_key_read
TESTING_REGRESS += col_wide_values agg_par_split col_vet_lazy_offsets
TESTING_REGRESS += lang_chain lang_memo col_wbuf_order_cache col_wbuf_big_set
TESTING_REGRESS += bm25_wbuf_read_promote bm25_wbuf_wide_bloom tbl_wide_rows col_wildcard_keys col_read_ahead col_dead_rewrite col_tomb_log col_track_updates_supersede col_track_shared_probe col_track_default col_track_deletes col_tomb_log_cache tbl_tomb_legacy_header col_tomb_log_cache
TESTING_REGRESS += col_point_exact col_point_packed col_point_root col_gorder_gaps col_dict_packed position_blocks agg_jsonb_not_exist col_document_present
TESTING_REGRESS += col_topn_unit_ends col_topn_deep_limits col_topn_text col_topn_raw col_topn_numeric col_topn_enum col_topn_interval col_topn_code col_topn_expr col_topn_arith col_topn_case col_topn_func col_topn_trail
# these assert the segments of a parallel build of an index with a key cut, which the release library builds in one process (build_pays in src/col/build_par.c)
TESTING_REGRESS += agg_numeric_in_list agg_shape_widths col_cost_selectivity_once col_cost_keys_sorted_once agg_source_cache agg_prepass_const_list
TESTING_REGRESS += col_gorder_chunks col_point_lazy col_point_radix col_merge_keys col_vacuum_parallel_keys col_volatile_notice agg_cal_paths col_rollup_auto col_cube_rank col_dict_cache col_seg_head col_vis_count col_zone_jnull col_dict_lookup col_dict_stored tbl_fetch_dict col_point_dir col_point_dense col_point_many_keys col_point_tid col_run_heads col_key_cut col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary col_sorted_run col_lazy_filter col_key_presence col_key_columns_auto agg_key_cube_fallback col_cube_priority col_paths_auto col_suggest_paths agg_preagg_elem agg_preagg_rollup col_rollup_refresh col_refresh col_approx col_gorder col_topn_gorder col_gorder_group col_key_table col_dict_bisect col_key_runs tbl_key_table col_dict_count col_point_chunks col_range_order col_gorder_auto col_dict_hash col_text_tsig col_text_contains col_scan_unit_order
TESTING_REGRESS += col_gorder_autogroup col_topn_gorder_filter col_topn_gorder_filter_text
TESTING_REGRESS += col_key_cut_paced col_list_test_once col_key_cut_ranges col_alloc_limit col_alloc_scaled
TESTING_REGRESS += col_gorder_narrow col_gorder_ends col_gorder_layout col_gorder_fullrange col_order_layout col_gorder_dead_cache
TESTING_REGRESS += col_gorder_text col_gorder_text_buffer col_gorder_textperm
TESTING_REGRESS += col_range_count_dirty
TESTING_REGRESS += col_text_ilike col_text_patterns col_text_prefilter
TESTING_REGRESS += col_dict_blocks_hash
TESTING_REGRESS += col_tomb_log_block_cache col_wbuf_read_priced col_wbuf_reads_write_path col_dsm_write_caches col_census_sample col_leaf_fold col_unit_floor agg_dict_blocks_lazy col_rewrite_cut agg_preagg_minmax col_auto_merge_partition col_auto_merge col_dead_delta col_holes_fuzz col_key_cut_partition col_key_cut_overlap col_key_cut_grown_overlay col_key_cut_redecide col_peers_dead_scan col_preagg_maint col_net_policy
TESTING_REGRESS += col_preagg_super
TESTING_REGRESS += col_alter_options
TESTING_REGRESS += agg_group_passes agg_group_spill agg_group_spill_limits agg_group_nospill_run agg_distinct_narrow agg_rows_group_memory agg_gord_group_skip_limit agg_par_blob_chunks col_ordered_budget col_dead_keys_budget
TESTING_REGRESS += agg_lookup_chain agg_par_partitions agg_partitions agg_partitions_many col_worker_failure col_range_count
TESTING_REGRESS += col_ioss_columns col_dict_shared
TESTING_REGRESS += col_text_scanwrap
TESTING_REGRESS += col_dict_shared_stale
TESTING_REGRESS += col_gorder_autogroup_kinds
TESTING_REGRESS += col_gorder_group_count
TESTING_REGRESS += store_sweep_runs tbl_tomb_log_limit tbl_wide_dict_limit tbl_vacuum_batches bm25_dead_windows
TESTING_REGRESS += col_gorder_count_big_group
TESTING_REGRESS += col_gorder_group_range col_build_same_options
TESTING_REGRESS += col_build_par_rows col_build_par_choice col_preagg
TESTING_REGRESS += col_gorder_group_points col_conn_journal col_conn_reindex col_conn_declare col_conn_inserted col_conn_carried col_conn_auto
TESTING_REGRESS += col_gorder_group_walk col_gorder_group_compact col_gorder_tiebreak col_key_cut_segment_cap col_topn_walk_price col_key_cut_hot_value col_key_cut_redecide_async
TESTING_REGRESS += col_topn_offset agg_child_warm agg_probe_direct agg_probe_direct_race agg_topn_offset_ties agg_topn_offset_dirty agg_topn_offset_bulk agg_topn_offset_keyed agg_topn_key_only
TESTING_REGRESS += col_unit_of_block agg_float_step
TESTING_REGRESS += tbl_hot_sift_index merge_parts merge_heal
TESTING_REGRESS += col_topn_text_offset col_gorder_text_count col_dsm_write_statements col_stmt_cid col_stmt_cid_bm25 col_track_rebaseline col_track_reindex_concurrently col_track_drop_concurrently
# Listed although scripts/check-testing-regress.sh would not select them (each fails on the release library, so the stock-library jobs must not run it):
# sift.test_run_docs, a setting only the testing build has
TESTING_REGRESS += build_memory build_parallel build_runs
# sift.test_par_workers, a setting only the testing build has
TESTING_REGRESS += parallel_build tbl_bulk_parallel
# sift.test_range_pieces and sift.test_range_bytes, settings only the testing build has
TESTING_REGRESS += build_ranges merge_ranges
# sift.test_page_text, a setting only the testing build has
TESTING_REGRESS += build_large_docs
# sift.grouped_min_df, a setting only the testing build has
TESTING_REGRESS += list_slice_seek rank_or_union rank_page_live
# sift.table_max_segments and sift.table_vblock_floor, settings only the testing build has
TESTING_REGRESS += tbl_bulk_groups tbl_cap tbl_vblocks
# an asserted value differs on the release library
TESTING_REGRESS += agg_preagg agg_preagg_rows agg_workers_few_units col_build_peers col_merge_memory
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
