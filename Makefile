EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
OBJS = $(patsubst %.c,%.o,$(wildcard $(SRC)/*/*.c $(SRC)/*/*/*.c))
DATA = $(wildcard sql/pg_sift--*.sql)
PGFILEDESC = "pg_sift - CTID-bitmap full-text search index"

# New SQL tests are registered automatically; the test-only patterns below apply to them too.
REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
# -MMD records header dependencies (src/*/*.d, src/*/*/*.d) so a header edit rebuilds its users.
PG_CPPFLAGS = $(addprefix -I$(srcdir)/,$(SIFT_INCDIRS))
PG_CFLAGS = -O2 -g -ffp-contract=off -MMD -Wno-declaration-after-statement $(SIFT_BASE_ISA)
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
TESTING_REGRESS = shred_% index_parts segfmt_% col_index_% store_% kernels_% col_kernels_% aggkern_% col_enc col_wbuf colcodec_% verify_% analyzer_version index_build_postings index_build_thresholds build_runs read_% build_memory build_limits build_pool build_page_text build_parallel aggstub_% maint_% tbl_cap tbl_bulk_eligible tbl_bulk_groups tbl_vblocks tbl_bigtx tbl_writepath tbl_page_corrupt col_format4 col_format5 col_tags agg_jsonb_typeof agg_jsonpath_kinds
TESTING_REGRESS += limits_registry tbl_limits_lowered agg_combo_cap agg_limit_refusals agg_planner_state_error
TESTING_REGRESS += errors_hints agg_cte_verdict_cache col_leaf_stats col_minmax_runs agg_outer_walks_once fuzzy_seek
TESTING_REGRESS += col_wide_values
TESTING_REGRESS += tbl_wide_rows col_wildcard_keys col_read_ahead col_dead_rewrite
TESTING_REGRESS += col_point_exact col_point_packed col_point_root col_gorder_gaps
TESTING_REGRESS += col_topn_unit_ends col_topn_deep_limits col_topn_text col_topn_raw col_topn_numeric col_topn_enum col_topn_interval col_topn_code col_topn_expr col_topn_arith col_topn_case col_topn_func col_topn_trail
TESTING_REGRESS += agg_numeric_in_list agg_shape_widths col_cost_selectivity_once col_cost_keys_sorted_once agg_source_cache agg_prepass_const_list
TESTING_REGRESS += col_gorder_chunks col_point_lazy col_point_radix col_merge_keys col_vacuum_parallel_keys col_volatile_notice col_pkey_notice agg_cal_paths col_rollup_auto col_cube_rank col_dict_cache col_seg_head col_vis_count col_zone_jnull col_dict_lookup col_dict_stored tbl_fetch_dict col_point_dir agg_lookup_keyed col_point_dense col_point_tid col_run_heads col_zone_window col_key_cut col_key_cut_units col_run_window col_point_filter agg_index_pick col_key_cut_advice col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary col_sorted_run col_lazy_filter col_key_presence col_key_columns_auto agg_key_cube_fallback col_cube_priority col_paths_auto col_suggest_paths agg_preagg_elem agg_preagg_elem_parallel agg_preagg_rollup col_rollup_refresh col_refresh parallel_build agg_row_compare col_skip_map col_topn_walk_cache col_approx col_gorder col_topn_gorder col_gorder_group col_topn_reverse_window col_key_table col_dict_bisect col_key_runs tbl_key_table col_dict_count col_point_chunks col_range_order col_gorder_auto col_dict_hash col_subplan_probe col_text_tsig col_text_contains
TESTING_REGRESS += col_gorder_chunks col_gorder_autogroup col_topn_ties col_point_lazy col_merge_keys col_vacuum_parallel_keys col_volatile_notice col_pkey_notice agg_cal_paths col_rollup_auto col_cube_rank col_dict_cache col_seg_head col_vis_count col_zone_jnull col_dict_lookup col_dict_stored tbl_fetch_dict col_point_dir agg_lookup_keyed col_point_dense col_point_tid col_run_heads col_zone_window col_key_cut col_key_cut_units col_run_window col_point_filter agg_index_pick col_key_cut_advice col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary col_sorted_run col_lazy_filter col_key_presence col_key_columns_auto agg_key_cube_fallback col_cube_priority col_paths_auto col_suggest_paths agg_preagg_elem agg_preagg_elem_parallel agg_preagg_rollup col_rollup_refresh col_refresh parallel_build agg_row_compare col_skip_map col_topn_walk_cache col_approx col_gorder col_topn_gorder col_gorder_group col_topn_reverse_window col_key_table col_dict_bisect col_key_runs tbl_key_table col_dict_count col_point_chunks col_range_order col_gorder_auto col_dict_hash col_subplan_probe col_text_tsig col_text_contains
TESTING_REGRESS += col_gorder_chunks col_point_lazy col_merge_keys col_vacuum_parallel_keys col_volatile_notice col_pkey_notice agg_cal_paths col_rollup_auto col_cube_rank col_dict_cache col_seg_head col_vis_count col_zone_jnull col_dict_lookup col_dict_stored tbl_fetch_dict col_point_dir agg_lookup_keyed col_point_dense col_point_tid col_run_heads col_zone_window col_key_cut col_key_cut_units col_run_window col_point_filter agg_index_pick col_key_cut_advice col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary col_sorted_run col_lazy_filter col_key_presence col_key_columns_auto agg_key_cube_fallback col_cube_priority col_paths_auto col_suggest_paths agg_preagg_elem agg_preagg_elem_parallel agg_preagg_rollup col_rollup_refresh col_refresh parallel_build agg_row_compare col_skip_map col_topn_walk_cache col_approx col_gorder col_topn_gorder col_gorder_group col_topn_reverse_window col_key_table col_dict_bisect col_key_runs tbl_key_table col_dict_count col_point_chunks col_range_order col_gorder_auto col_dict_hash col_subplan_probe col_text_tsig col_text_contains col_key_cut_narrow
TESTING_REGRESS += col_gorder_chunks col_gorder_autogroup col_gorder_narrow col_point_lazy col_merge_keys col_vacuum_parallel_keys col_volatile_notice col_pkey_notice agg_cal_paths col_rollup_auto col_cube_rank col_dict_cache col_seg_head col_vis_count col_zone_jnull col_dict_lookup col_dict_stored tbl_fetch_dict col_point_dir agg_lookup_keyed col_point_dense col_point_tid col_run_heads col_zone_window col_key_cut col_key_cut_units col_run_window col_point_filter agg_index_pick col_key_cut_advice col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary col_sorted_run col_lazy_filter col_key_presence col_key_columns_auto agg_key_cube_fallback col_cube_priority col_paths_auto col_suggest_paths agg_preagg_elem agg_preagg_elem_parallel agg_preagg_rollup col_rollup_refresh col_refresh parallel_build agg_row_compare col_skip_map col_topn_walk_cache col_approx col_gorder col_topn_gorder col_gorder_group col_topn_reverse_window col_key_table col_dict_bisect col_key_runs tbl_key_table col_dict_count col_point_chunks col_range_order col_gorder_auto col_dict_hash col_subplan_probe col_text_tsig col_text_contains
TESTING_REGRESS += col_gorder_chunks col_gorder_autogroup col_point_lazy col_merge_keys col_vacuum_parallel_keys col_volatile_notice col_pkey_notice agg_cal_paths col_rollup_auto col_cube_rank col_dict_cache col_seg_head col_vis_count col_zone_jnull col_dict_lookup col_dict_stored tbl_fetch_dict col_point_dir agg_lookup_keyed col_point_dense col_point_tid col_run_heads col_zone_window col_key_cut col_key_cut_units col_run_window col_point_filter agg_index_pick col_key_cut_advice col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary col_sorted_run col_lazy_filter col_key_presence col_key_columns_auto agg_key_cube_fallback col_cube_priority col_paths_auto col_suggest_paths agg_preagg_elem agg_preagg_elem_parallel agg_preagg_rollup col_rollup_refresh col_refresh parallel_build agg_row_compare col_skip_map col_topn_walk_cache col_approx col_gorder col_topn_gorder col_gorder_group col_topn_reverse_window col_key_table col_dict_bisect col_key_runs tbl_key_table col_dict_count col_point_chunks col_range_order col_gorder_auto col_dict_hash col_subplan_probe col_text_tsig col_text_contains
TESTING_REGRESS += col_gorder_text
TESTING_REGRESS += col_date_bounds
TESTING_REGRESS += col_plain_window col_text_ilike col_text_patterns col_text_prefilter
TESTING_REGRESS += col_plain_window col_text_ilike col_text_patterns
TESTING_REGRESS += col_dict_blocks_hash
TESTING_REGRESS += col_census_sample col_unit_floor col_key_cut_auto agg_dict_blocks_lazy
TESTING_REGRESS += col_preagg_super
TESTING_REGRESS += col_alter_options
TESTING_REGRESS += agg_group_spill_limits agg_distinct_narrow agg_distinct_sort
TESTING_REGRESS += agg_lookup_chain agg_par_partitions agg_partitions agg_partitions_many col_worker_failure col_range_count
TESTING_REGRESS += col_ioss_columns col_dict_shared
TESTING_REGRESS += col_text_scanwrap
TESTING_REGRESS += col_ioss_columns col_dict_shared col_dict_shared_stale
TESTING_REGRESS += col_point_auto
TESTING_REGRESS += col_ioss_columns col_dict_shared col_dict_shared_stale agg_explain_why_scan
TESTING_REGRESS += col_topn_offset agg_child_warm
TESTING_REGRESS += col_unit_of_block
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
