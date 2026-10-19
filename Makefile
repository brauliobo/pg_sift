EXTENSION = pg_sift
MODULE_big = pg_sift
SIFT_MK := $(lastword $(MAKEFILE_LIST))
SRC = src
include $(SRC)/areas.mk
OBJS = $(patsubst %.c,%.o,$(wildcard $(SRC)/*/*.c))
DATA = $(wildcard pg_sift--*.sql)
PGFILEDESC = "pg_sift - CTID-bitmap full-text search index"

REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
# -MMD records header dependencies (src/*/*.d) so a header edit rebuilds its users.
PG_CPPFLAGS = $(addprefix -I$(srcdir)/,$(SIFT_DIRS))
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
# The general-purpose layers of the column codec (src/col/sift_colz.c): LZ4 and zstd, each linked when its header and library are found.
# SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 build without one: chunks never use it, and one written elsewhere that does fails to open with a clear error.
define colz_probe
$(if $(SIFT_NO_$(1)),,$(if $(shell echo 'int main(void) { return 0; }' | $(CC) -include $(2).h -x c - -l$(2) -o /dev/null 2>/dev/null && echo ok),$(eval PG_CPPFLAGS += -DSIFT_HAVE_$(1))$(eval SHLIB_LINK += -l$(2))))
endef
$(call colz_probe,LZ4,lz4)
$(call colz_probe,ZSTD,zstd)
EXTRA_CLEAN = src/*/*.d .build-flavor test/results test/regression.diffs test/regression.out
ifdef SANITIZE
PG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SHLIB_LINK += -fsanitize=address,undefined
endif

# SIFT_TESTING=1 compiles the sift._test_* functions and installs the
# test-only pg_sift_test extension; regress tests using it are skipped otherwise.
# Switching it needs `make clean`.
TESTING_REGRESS = shred_% index_parts segfmt_% col_index_% store_% kernels_% col_kernels_% aggkern_% col_enc col_wbuf colcodec_% verify_% analyzer_version index_build_postings index_build_thresholds build_runs read_% build_memory build_limits build_pool build_page_text build_parallel aggstub_% maint_% tbl_cap tbl_vblocks
ifdef SIFT_TESTING
EXTENSION += pg_sift_test
DATA += pg_sift_test--1.0.sql
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
.PHONY: isolationcheck tapcheck difftest fuzz soak
isolationcheck:
	scripts/isolation.sh
tapcheck:
	scripts/tap.sh
difftest fuzz soak:
	scripts/$@.sh
