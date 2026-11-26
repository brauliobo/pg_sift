# The pure-C unit harness of the engine (no backend linked), mirroring src/: a test is the file of src/ it covers, under test/unit/ (core/par/arena.c tests
# src/core/par/arena.c), built on the stand-ins of test/common with the palloc/MemoryContext/ereport calls trapped on the threads a test flags as workers
# (test/common/trapshim.h).
#   make all                           the tests and benchmarks, optimized
#   make run-plain|run-asan|run-tsan   build and run every test: plain, ASan+UBSan, ThreadSanitizer (-jN: N at a time, the output of each in one piece)
#   make check-PATH                    build and run one test (MODE=asan|tsan: under that sanitizer)
#   make bench                         the benchmarks, under flock /tmp/sift-perf.lock
#   make PG_CONFIG=/path/to/pg_config  against another PostgreSQL
# A test is PATH.c, listed in TESTS, with LIB_PATH = <sources of src/ it links> and FX_PATH = <objects of this directory it links>; a benchmark the same in
# BENCHES. A test that includes the sources it covers (the Format 2 ones) lists itself in AUTOSTUB: the symbols of the server it does not run link as
# stubs that trap. The set-up thread allocates with pgshim and the workers (unit_run_workers of check.h) must not.
# The tests of a subsystem come from one mk/*.mk.
include ../common/rules.mk
OUT     ?= /tmp/sift-unit
MODE    ?= plain
B        = $(OUT)/$(MODE)

TESTS    =
BENCHES  =
AUTOSTUB =

CFLAGS   = -std=gnu17 -g -fno-omit-frame-pointer $(OPT_$(MODE)) $(SIFT_BASE_ISA) -ffp-contract=off -Wall -Werror -pthread \
           $(SIFT_INC) $(SIFT_TEST_INC) -I$(PGINC) $(SIFT_PGCPP) -I. -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD
WRAPPED  = ${shell sed -n 's/^TRAPV\?([^,]*, *\([A-Za-z0-9_]*\),.*/\1/p' $(COMMON)/trapped.def}
DEPFLAGS = -MMD -MP
LDFLAGS  = $(foreach f,$(WRAPPED) pthread_create,-Wl,--wrap=$(f))
LDLIBS   = $(PGLIBS) -lzstd -llz4 -lm
SHIM     = $(B)/pgshim.o $(B)/trapshim.o
objs     = $(patsubst %.c,$(B)/lib/%.o,$(LIB_$(1)))

include $(wildcard mk/*.mk)

all: $(addprefix $(B)/,$(TESTS) $(BENCHES))

$(B)/lib/%.o: $(SRC)/%.c
	@mkdir -p $(@D)
	$(CCACHE) $(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(eval $(call common_obj,$(B),pgshim))
$(eval $(call common_obj,$(B),trapshim))
$(eval $(call common_obj,$(B),cpushim))

$(B)/%.o: %.c
	@mkdir -p $(@D)
	$(CCACHE) $(CC) $(CFLAGS) $(XFLAGS_$*) $(DEPFLAGS) -c -o $@ $<

link = $(CC) $(CFLAGS) -o $@ $< $(call objs,$*) $(SHIM) $(FX_$*) $(LDFLAGS) $(LDLIBS)
stubs = grep -o "undefined reference to \`[A-Za-z0-9_]*'" $@.log | sed "s/.*\`//; s/'//" | sort -u | sed 's/.*/void &(void) { __builtin_trap(); }/' >$@.stubs.c
autolink = LC_ALL=C $(link) 2>$@.log || { $(stubs) && $(link) $@.stubs.c; }

.SECONDEXPANSION:
$(B)/%: $(B)/%.o $$(call objs,$$*) $(SHIM) $$(FX_$$*) $(COMMON)/trapped.def
	$(if $(filter $*,$(AUTOSTUB)),$(autolink),$(link))

test: $(addprefix check-,$(TESTS))

$(addprefix check-,$(TESTS)): check-%: $(B)/%
	@echo "== $(MODE) $*"; $(ENV_$(MODE)) $< || { echo "FAILED $(MODE) $*"; exit 1; }

run-%:
	$(MAKE) -k -O MODE=$* test

asan: run-asan
tsan: run-tsan

bench: $(addprefix $(B)/,$(BENCHES))
	@for b in $(BENCHES); do $(FLOCK) $(B)/$$b || exit 1; done

clean:
	rm -rf $(OUT)

-include $(shell find $(B) -name '*.d' 2>/dev/null)

.DELETE_ON_ERROR:
.SECONDARY:
.PHONY: all test $(addprefix check-,$(TESTS)) run-% asan tsan bench clean
