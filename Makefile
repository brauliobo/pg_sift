EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
OBJS = $(patsubst %.c,%.o,$(wildcard $(SRC)/*/*.c))
DATA = $(wildcard sql/pg_sift--*.sql)
PGFILEDESC = "pg_sift - CTID-bitmap full-text search index"

# New SQL tests are registered automatically; the test-only patterns below apply to them too.
REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
# -MMD records header dependencies (src/*/*.d) so a header edit rebuilds its users.
PG_CPPFLAGS = $(addprefix -I$(srcdir)/,$(SIFT_INCDIRS))
PG_CFLAGS = -O2 -g -ffp-contract=off -MMD -Wno-declaration-after-statement
ifeq ($(shell uname -m),x86_64)
PG_CFLAGS += -march=x86-64 -mtune=generic
endif
# PGXS inherits -flto=auto from some distributions' pg_config: a link step that starts one job per core.
# Off by default; SIFT_LTO=1 enables it with at most 4 jobs. Switching it needs `make clean`.
ifdef SIFT_LTO
PG_CFLAGS += -flto=4
SHLIB_LINK += -flto=4
else
PG_CFLAGS += -fno-lto
SHLIB_LINK += -fno-lto
endif
# The general-purpose layers of the column codec (src/col/compress.c): LZ4 and zstd, each linked when its header and library are found.
# SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 build without one: chunks never use it, and one written elsewhere that does fails to open with a clear error.
define colz_probe
$(if $(SIFT_NO_$(1)),,$(if $(shell echo 'int main(void) { return 0; }' | $(CC) -include $(2).h -x c - -l$(2) -o /dev/null 2>/dev/null && echo ok),$(eval PG_CPPFLAGS += -DSIFT_HAVE_$(1))$(eval SHLIB_LINK += -l$(2))))
endef
$(call colz_probe,LZ4,lz4)
$(call colz_probe,ZSTD,zstd)
# VULKAN=1 (or SIFT_VULKAN=1) compiles the opt-in Vulkan compute backend (docs/GPU.md): libvulkan is opened with dlopen at first use, so there is no
# link-time dependency beyond -ldl, and the Vulkan declarations and the SPIR-V are checked in (no SDK, no shader compiler). Off by default.
# Switching it needs `make clean`.
ifneq ($(or $(VULKAN),$(SIFT_VULKAN)),)
PG_CPPFLAGS += -DSIFT_VULKAN
SHLIB_LINK += -ldl
endif
EXTRA_CLEAN = src/*/*.d .build-flavor test/results test/regression.diffs test/regression.out
ifdef SANITIZE
PG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SHLIB_LINK += -fsanitize=address,undefined
endif

# SIFT_TESTING=1 compiles the sift._test_* functions and installs the
# test-only pg_sift_test extension; regress tests using it are skipped otherwise.
# Switching it needs `make clean`.
TESTING_REGRESS = shred_% index_parts segfmt_% col_index_% store_% kernels_% col_kernels_% aggkern_% col_enc col_wbuf colcodec_% verify_% analyzer_version index_build_postings index_build_thresholds build_runs read_% build_memory build_limits build_pool build_page_text build_parallel aggstub_% maint_% tbl_cap tbl_bulk_eligible tbl_bulk_groups tbl_vblocks tbl_bigtx tbl_writepath tbl_page_corrupt col_format4 col_format5 col_tags agg_jsonb_typeof agg_jsonpath_kinds
TESTING_REGRESS += limits_registry tbl_limits_lowered agg_combo_cap agg_limit_refusals
TESTING_REGRESS += errors_hints
TESTING_REGRESS += col_wide_values
TESTING_REGRESS += tbl_wide_rows col_wildcard_keys
TESTING_REGRESS += col_topn_deep_limits
TESTING_REGRESS += agg_numeric_in_list agg_shape_widths
TESTING_REGRESS += col_dict_cache col_dict_lookup col_damage col_preagg_elem col_preagg_minmax col_preagg_calendar col_calendar_columns tbl_preagg_calendar col_preagg_rows col_set_summary agg_preagg_elem agg_preagg_elem_parallel agg_preagg_rollup col_rollup_refresh col_refresh parallel_build
TESTING_REGRESS += col_preagg_super
ifdef SIFT_TESTING
EXTENSION += pg_sift_test
DATA += sql/pg_sift_test--1.0.sql
PG_CPPFLAGS += -DSIFT_TESTING
else
REGRESS := $(filter-out $(TESTING_REGRESS),$(REGRESS))
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

# make check / check-full only run scripts/ci.sh, which builds and installs on its own; PGXS would
# claim "check" is unsupported, so it is not included for these goals. pg-assert and pg-valgrind build the PostgreSQL of the `assert`
# and `valgrind` CI lanes once into a cache outside the repo (scripts/pg-assert.sh; PG_ASSERT_MAJOR=17 for the 17 branch).
ifneq ($(filter check check-full pg-assert pg-valgrind,$(MAKECMDGOALS)),)
.PHONY: check check-full pg-assert pg-valgrind
check:
	scripts/ci.sh quick
check-full:
	scripts/ci.sh full
pg-assert:
	nice scripts/pg-assert.sh $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
pg-valgrind:
	nice scripts/pg-assert.sh --valgrind $(if $(PG_ASSERT_MAJOR),--major $(PG_ASSERT_MAJOR))
else
include $(PGXS)
endif

-include $(wildcard src/*/*.d)

# Suites beyond installcheck, run against the scripts/pgtest.sh cluster
# (install and start it first). scripts/ci.sh runs all of them.
.PHONY: isolationcheck tapcheck difftest fuzz soak gpu-shaders limits refusals reference
isolationcheck:
	scripts/isolation.sh
tapcheck:
	scripts/tap.sh
difftest fuzz soak:
	scripts/$@.sh
# docs/LIMITS.md from the registry in src/core/limits.h (scripts/gen-limits.sh --check proves it)
limits:
	scripts/gen-limits.sh
# the slug tables of docs/USAGE.md and docs/COL-PLAN.md from src/agg/refuse.h (scripts/gen-refusals.sh --check proves them and the planner sources)
refusals:
	scripts/gen-refusals.sh
# docs/REFERENCE.md and the README block from the sift.* objects of the install scripts (scripts/gen-reference.sh --check proves them)
reference:
	scripts/gen-reference.sh

# Rebuilds src/agg/vk_spv.h from the shader (needs glslc) and the Vulkan declarations from /usr/include/vulkan; commit the result.
gpu-shaders:
	scripts/gen-aggvk-spv.sh
	scripts/gen-aggvk-min.py
