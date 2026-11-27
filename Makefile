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
include $(dir $(SIFT_MK))scripts/testing-regress.mk
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
