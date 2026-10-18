# Column codec research harness, tests and benchmark (no backend linked).
#   nice make -j4              build test and bench into $(OUT) (the research tool is opt-in: make research)
#   make research ARGS='-s'    encoding/compressor sizes on order-shaped data (-s adds decode speeds, -k clustered order)
#   make test [N=2]            round trips, adversarial data, corruption fuzz, per-level kernel differential
#   make SANITIZE=1 test       ASan + UBSan build (into $(OUT)-san)
#   make bench                 ratios per policy and decode/filter speeds per SIMD level (under flock /tmp/sift-perf.lock)
# The cpu module stand-in and the random generator are tools/kernels' (shim.c, harness.h); the column kernels the codec runs on are src/sift_col_kern*.c.
# The chunk generators of test.c are src/sift_colcodec_gen.h, the ones of the in-backend regress test (test/sql/colcodec_*.sql) too.
CC      ?= gcc
SRC      = ../../src
KT       = ../kernels
PGINC   := $(shell pg_config --includedir-server)
PGLIB   := -L$(shell pg_config --libdir)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -march=x86-64 -mtune=generic -ffp-contract=off -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
           -Wno-sign-compare -I$(SRC) -I$(KT) -I$(PGINC) -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD
LDLIBS   = -lm -lzstd -llz4
OUT     ?= /tmp/sift-col-compress/out
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OUT     := $(OUT)-san
endif
N       ?= 1

CODEC    = $(addprefix $(SRC)/,sift_colcodec.c sift_colcodec_kern.c sift_colcodec_avx2.c sift_colcodec_avx512.c sift_colz.c \
                               sift_kernels.c sift_kernels_avx2.c sift_kernels_avx512.c sift_analyze.c sift_analyze_stem.c \
                               sift_col_kern.c sift_col_kern_avx2.c sift_col_kern_avx512.c sift_bitmap_scalar.c sift_bitmap_avx2.c sift_bitmap_avx512.c) $(KT)/shim.c
OBJ      = $(patsubst %.c,$(OUT)/%.o,$(notdir $(CODEC))) $(OUT)/ccshim.o $(OUT)/pgshim.o
HDR      = $(wildcard $(SRC)/sift_colcodec*.h $(SRC)/sift_colz.h $(SRC)/sift_kernels*.h $(SRC)/sift_analyze*.h $(SRC)/sift_col_kern*.h $(SRC)/sift_col.h) ccshim.h

all: $(OUT)/test $(OUT)/bench

$(OUT)/research: research.c gen.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ research.c $(LDLIBS)

vpath %.c $(SRC) $(KT)

$(OUT)/pgshim.o: ../pgshim.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: test.c $(SRC)/sift_colcodec_gen.h $(OBJ) $(HDR)
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
