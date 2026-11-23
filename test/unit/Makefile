# The pure-C unit harness of the engine threads (no backend linked): the sources of src/ that a pool thread runs, built on tools/pgshim.c, with the
# palloc/MemoryContext/ereport calls trapped on the threads a test flags as workers (trapshim.h).
#   make all                   the tests and benchmarks, optimized
#   make run-plain|run-asan|run-tsan   build and run the tests: plain, ASan+UBSan, ThreadSanitizer (asan, tsan: the same)
#   make bench                 the benchmarks, under flock /tmp/sift-perf.lock
# A test is test_X.c and a line LIB_test_X = <sources of src/ it links>; a benchmark the same with bench_X. The set-up thread allocates with
# pgshim and the workers (unit_run_workers of unit.h) must not.
CC      ?= gcc
SRC      = ../../src
TOOLS    = ../../tools
include $(SRC)/areas.mk
PGINC   := $(shell pg_config --includedir-server)
PGLIB   := -L$(shell pg_config --libdir)
FLOCK    = $(if $(shell command -v flock),flock /tmp/sift-perf.lock)
OUT     ?= /tmp/sift-unit
MODE    ?= plain

TESTS   = test_arena
BENCHES = bench_arena
LIB_test_arena  = core/par/arena.c col/compress.c
LIB_bench_arena = core/par/arena.c

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
LDFLAGS  = $(foreach f,$(WRAPPED),-Wl,--wrap=$(f))
LDLIBS   = $(PGLIB) -lpgport $(SIFT_PGLIBS) -lzstd -llz4 -lm
HDR      = $(wildcard $(SRC)/core/*.h $(SRC)/col/*.h) $(wildcard *.h) trapped.def
SHIM     = $(B)/pgshim.o $(B)/trapshim.o
objs     = $(patsubst %.c,$(B)/lib/%.o,$(LIB_$(1)))

-include $(wildcard *.mk)
TESTS += $(GRANULE_TESTS)

all: $(addprefix $(B)/,$(TESTS) $(BENCHES))

$(B)/lib/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/pgshim.o: $(TOOLS)/pgshim.c $(TOOLS)/pgshim.h $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS) -c -o $@ $<

$(B)/trapshim.o: trapshim.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

.SECONDEXPANSION:
$(B)/%: %.c $$(call objs,$$*) $(SHIM) $(HDR)
	$(CC) $(CFLAGS) -I$(TOOLS) -o $@ $< $(call objs,$*) $(SHIM) $(LDFLAGS) $(LDLIBS)

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

# ---- test_codec: the chunk codec of tools/colcodec on pool threads (the same sources, the cpu module of tools/kernels/shim.c)
CODEC_SRC = col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec_policy.c col/codec_write.c col/codec_parse.c col/codec_blob.c \
            col/codec_cold.c col/codec_kern.c col/codec_avx2.c col/codec_avx512.c col/codec_neon.c col/compress.c col/fsst.c col/codec_a.c \
            bm25/kernels.c bm25/kernels_avx2.c bm25/kernels_avx512.c bm25/kernels_neon.c bm25/analyze.c bm25/analyze_config.c \
            bm25/analyze_stem.c col/v1/kern.c col/v1/kern_avx2.c col/v1/kern_avx512.c col/v1/kern_neon.c bm25/bitmap_scalar.c \
            bm25/bitmap_avx2.c bm25/bitmap_avx512.c bm25/bitmap_neon.c
TESTS += test_codec
LIB_test_codec = core/arena.c $(CODEC_SRC)

all: $(B)/test_codec
test: $(B)/test_codec

$(B)/kshim.o: $(TOOLS)/kernels/shim.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS)/kernels -c -o $@ $<

$(B)/test_codec: test_codec.c $(call objs,test_codec) $(SHIM) $(B)/kshim.o $(HDR) $(SRC)/col/codec_gen.h
	$(CC) $(CFLAGS) -I$(TOOLS) -o $@ $< $(call objs,test_codec) $(SHIM) $(B)/kshim.o $(LDFLAGS) $(LDLIBS)
