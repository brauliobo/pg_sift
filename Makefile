EXTENSION = sift
MODULE_big = sift
OBJS = $(patsubst %.c,%.o,$(wildcard src/*.c))
DATA = sift--1.0.sql
PGFILEDESC = "sift - CTID-bitmap full-text search index"

REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test

# Portable baseline: SIMD kernels opt in per function with target attributes.
# -ffp-contract=off keeps scalar and SIMD scores bit-identical.
PG_CPPFLAGS = -I$(srcdir)/src
PG_CFLAGS = -march=x86-64 -mtune=generic -O2 -g -ffp-contract=off
ifdef SANITIZE
PG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
SHLIB_LINK += -fsanitize=address,undefined
endif

# SIFT_TESTING=1 compiles the sift._test_* functions and installs the
# test-only sift_test extension; regress tests using it are skipped otherwise.
# Switching it needs `make clean`.
TESTING_REGRESS = store_%
ifdef SIFT_TESTING
EXTENSION += sift_test
DATA += sift_test--1.0.sql
PG_CPPFLAGS += -DSIFT_TESTING
else
REGRESS := $(filter-out $(TESTING_REGRESS),$(REGRESS))
endif

# No LLVM bitcode: avoids a clang dependency and -march=native bitcode.
override with_llvm := no

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Suites beyond installcheck, run against the scripts/pgtest.sh cluster
# (install and start it first). scripts/ci.sh runs all of them.
.PHONY: isolationcheck tapcheck difftest fuzz soak ci
isolationcheck:
	scripts/isolation.sh
tapcheck:
	scripts/tap.sh
difftest fuzz soak:
	scripts/$@.sh
ci:
	scripts/ci.sh
