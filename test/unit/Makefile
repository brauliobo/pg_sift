# The pure-C unit harness of the engine threads (no backend linked): the sources of src/ that a pool thread runs, built on tools/pgshim.c, with the
# palloc/MemoryContext/ereport calls trapped on the threads a test flags as workers (trapshim.h).
#   make all                   the tests and benchmarks, optimized
#   make run-plain|run-asan|run-tsan   build and run the tests: plain, ASan+UBSan, ThreadSanitizer (asan, tsan: the same)
#   make bench                 the benchmarks, under flock /tmp/sift-perf.lock
# A test is test_X.c, listed in TESTS, with LIB_test_X = <sources of src/ it links> and FX_test_X = <objects of this directory it links>; a benchmark
# the same with bench_X in BENCHES. The set-up thread allocates with pgshim and the workers (unit_run_workers of unit.h) must not.
# The tests of a kind come from one *.mk: codec.mk, granule.mk (the tests that decode real granules), filter.mk.
CC      ?= gcc
SRC      = ../../src
TOOLS    = ../../tools
include $(SRC)/areas.mk
PGINC   := $(shell pg_config --includedir-server)
PGLIB   := -L$(shell pg_config --libdir)
FLOCK    = $(if $(shell command -v flock),flock /tmp/sift-perf.lock)
OUT     ?= /tmp/sift-unit
MODE    ?= plain

TESTS   = test_arena par_core par_radix
BENCHES = bench_arena bench_par_radix
LIB_test_arena  = core/par/arena.c col/compress.c
LIB_bench_arena = core/par/arena.c
LIB_par_core    = core/par/arena.c core/par/morsel.c core/par/pool.c core/par/radix.c core/par/topk.c
FX_par_core     = $(B)/par_host.o
LIB_par_radix   = $(LIB_par_core) agg/v6/par/group.c agg/v6/par/distinct.c agg/v6/v6_group_cmp.c agg/v6/v6_distinct_u64.c agg/kern6_hash.c
FX_par_radix    = $(B)/par_host.o
LIB_bench_par_radix = $(LIB_par_radix)
FX_bench_par_radix  = $(B)/par_host.o

OPT_plain = -O2
OPT_asan  = -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined
OPT_tsan  = -O1 -fsanitize=thread
ENV_plain =
ENV_asan  = ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1
ENV_tsan  = TSAN_OPTIONS=halt_on_error=1:exitcode=66

B        = $(OUT)/$(MODE)
CFLAGS   = -std=gnu17 -g -fno-omit-frame-pointer $(OPT_$(MODE)) $(SIFT_BASE_ISA) -ffp-contract=off -Wall -Werror -pthread \
           $(SIFT_INC) -I$(PGINC) $(SIFT_PGCPP) -I. -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD
WRAPPED  = ${shell sed -n 's/^TRAPV\?([^,]*, *\([A-Za-z0-9_]*\),.*/\1/p' trapped.def}
LDFLAGS  = $(foreach f,$(WRAPPED) pthread_create,-Wl,--wrap=$(f))
LDLIBS   = $(PGLIB) -lpgport -lpgcommon $(SIFT_PGLIBS) -lzstd -llz4 -lm
HDR      = $(wildcard $(SRC)/core/*.h $(SRC)/col/*.h) $(wildcard *.h) trapped.def
SHIM     = $(B)/pgshim.o $(B)/trapshim.o
objs     = $(patsubst %.c,$(B)/lib/%.o,$(LIB_$(1)))

-include $(wildcard *.mk)

all: $(addprefix $(B)/,$(TESTS) $(BENCHES))

$(B)/lib/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/pgshim.o: $(TOOLS)/pgshim.c $(TOOLS)/pgshim.h $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS) -c -o $@ $<

$(B)/kshim.o: $(TOOLS)/kernels/shim.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS)/kernels -c -o $@ $<

$(B)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS) -c -o $@ $<

.SECONDEXPANSION:
$(B)/%: %.c $$(call objs,$$*) $(SHIM) $$(FX_$$*) $(HDR)
	$(CC) $(CFLAGS) -I$(TOOLS) -o $@ $< $(call objs,$*) $(SHIM) $(FX_$*) $(LDFLAGS) $(LDLIBS)

test: $(addprefix $(B)/,$(TESTS))
	@for t in $(TESTS); do echo "== $(MODE) $$t"; $(ENV_$(MODE)) $(B)/$$t || exit 1; done

run-%:
	$(MAKE) MODE=$* test

asan: run-asan
tsan: run-tsan

bench: $(addprefix $(B)/,$(BENCHES))
	@for b in $(BENCHES); do $(FLOCK) $(B)/$$b || exit 1; done

clean:
	rm -rf $(OUT)

.PHONY: all test run-% asan tsan bench clean
