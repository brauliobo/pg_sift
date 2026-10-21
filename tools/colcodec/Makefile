# Column codec research harness, tests and benchmark (no backend linked).
#   nice make -j4              build test and bench into $(OUT) (the research tool is opt-in: make research)
#   make research ARGS='-s'    encoding/compressor sizes on order-shaped data (-s adds decode speeds, -k clustered order)
#   make test [N=2]            round trips, adversarial data, corruption fuzz, per-level kernel differential
#   make SANITIZE=1 test       ASan + UBSan build (into $(OUT)-san)
#   make bench                 ratios per policy and decode/filter speeds per SIMD level (under flock /tmp/sift-perf.lock)
# The cpu module stand-in and the random generator are tools/kernels' (shim.c, harness.h); the column kernels the codec runs on are src/col/kern*.c.
# The chunk generators of test.c are src/col/codec_gen.h, the ones of the in-backend regress test (test/sql/colcodec_*.sql) too.
CC      ?= gcc
SRC      = ../../src
include $(SRC)/areas.mk
KT       = ../kernels
PGINC   := $(shell pg_config --includedir-server)
PGLIB   := -L$(shell pg_config --libdir)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -march=x86-64 -mtune=generic -ffp-contract=off -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
           -Wno-sign-compare $(SIFT_INC) -I$(KT) -I$(PGINC) -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD
LDLIBS   = -lm -lzstd -llz4
OUT     ?= /tmp/sift-col-compress/out
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OUT     := $(OUT)-san
endif
N       ?= 1

CODEC    = $(addprefix $(SRC)/,col/codec.c col/codec_policy.c col/codec_write.c col/codec_parse.c col/codec_blob.c col/codec_kern.c col/codec_avx2.c col/codec_avx512.c col/compress.c \
                               bm25/kernels.c bm25/kernels_avx2.c bm25/kernels_avx512.c bm25/analyze.c bm25/analyze_config.c bm25/analyze_stem.c \
                               col/kern.c col/kern_avx2.c col/kern_avx512.c bm25/bitmap_scalar.c bm25/bitmap_avx2.c bm25/bitmap_avx512.c)
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(CODEC)) $(OUT)/shim.o $(OUT)/ccshim.o $(OUT)/pgshim.o
HDR      = $(wildcard $(SRC)/col/codec*.h $(SRC)/col/compress.h $(SRC)/bm25/kernels*.h $(SRC)/bm25/analyze*.h $(SRC)/col/kern*.h $(SRC)/col/col.h) ccshim.h

all: $(OUT)/test $(OUT)/bench

$(OUT)/research: research.c research_enc.h research_float.h gen.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ research.c $(LDLIBS)

$(OUT)/pgshim.o: ../pgshim.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/shim.o: $(KT)/shim.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: test.c $(SRC)/col/codec_gen.h $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ test.c $(OBJ) $(PGLIB) -lpgport $(LDLIBS)

$(OUT)/bench: bench.c gen.h $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ bench.c $(OBJ) $(PGLIB) -lpgport $(LDLIBS)

research: $(OUT)/research
	flock /tmp/sift-perf.lock $(OUT)/research $(ARGS)

test: $(OUT)/test
	$(OUT)/test $(N)

bench: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench $(ARGS)

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: all research test bench clean
