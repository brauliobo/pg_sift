EXTENSION = pg_sift
MODULE_big = pg_sift
OBJS = $(patsubst %.c,%.o,$(wildcard src/*.c))
DATA = $(wildcard pg_sift--*.sql)
PGFILEDESC = "pg_sift - CTID-bitmap full-text search index"

REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
# -MMD records header dependencies (src/*.d) so a header edit rebuilds its users.
PG_CPPFLAGS = -I$(srcdir)/src
PG_CFLAGS = -O2 -g -ffp-contract=off -MMD -Wno-declaration-after-statement
ifeq ($(shell uname -m),x86_64)
PG_CFLAGS += -march=x86-64 -mtune=generic
endif
EXTRA_CLEAN = src/*.d .build-flavor test/results test/regression.diffs test/regression.out
ifdef SANITIZE
PG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SHLIB_LINK += -fsanitize=address,undefined
endif

# SIFT_TESTING=1 compiles the sift._test_* functions and installs the
# test-only pg_sift_test extension; regress tests using it are skipped otherwise.
# Switching it needs `make clean`.
TESTING_REGRESS = store_% kernels_% verify_% analyzer_version index_build_postings index_build_thresholds build_runs build_memory build_limits build_pool build_page_text build_parallel
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

# make check / check-full only run scripts/ci.sh, which builds and installs on its own; PGXS would
# claim "check" is unsupported, so it is not included for these goals.
ifneq ($(filter check check-full,$(MAKECMDGOALS)),)
.PHONY: check check-full
check:
	scripts/ci.sh quick
check-full:
	scripts/ci.sh full
else
include $(PGXS)
endif

-include $(wildcard src/*.d)

# Suites beyond installcheck, run against the scripts/pgtest.sh cluster
# (install and start it first). scripts/ci.sh runs all of them.
.PHONY: isolationcheck tapcheck difftest fuzz soak
isolationcheck:
	scripts/isolation.sh
tapcheck:
	scripts/tap.sh
difftest fuzz soak:
	scripts/$@.sh
