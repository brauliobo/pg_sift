# Column codec research harness, tests and benchmark (no backend linked).
#   nice make -j4              build test and bench into $(OUT) (the research tool is opt-in: make research)
#   make research ARGS='-s'    encoding/compressor sizes on order-shaped data (-s adds decode speeds, -k clustered order)
#   make test [N=2]            round trips, adversarial data, corruption fuzz, per-level kernel differential
#   make SANITIZE=1 test       ASan + UBSan build (into $(OUT)-san)
#   make bench                 ratios per policy and decode/filter speeds per SIMD level (under flock /tmp/sift-perf.lock)
# The cpu module stand-in is test/common/cpushim.c, the random generator tools/kernels' (harness.h); the column kernels the codec runs on are src/col/v1/kern/kern*.c.
# The chunk generators of test.c are src/col/codec/gen.h, the ones of the in-backend regress test (test/sql/colcodec_*.sql) too.
include ../../test/common/rules.mk
KT       = ../kernels
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 $(SIFT_BASE_ISA) -ffp-contract=off -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
           -Wno-sign-compare $(SIFT_INC) $(SIFT_TEST_INC) -I$(KT) -I$(PGINC) $(SIFT_PGCPP) -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD
LDLIBS   = -lm $(SIFT_PGLIBS) -lzstd -llz4
OUT     ?= /tmp/sift-col-compress/out
ifdef SANITIZE
CFLAGS  += $(SAN_ASAN)
OUT     := $(OUT)-san
endif
N       ?= 1

CODEC    = $(addprefix $(SRC)/,core/par/arena.c col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec/policy.c col/codec/write.c col/codec/parse.c col/codec/blob.c col/codec/cold.c col/codec/kern.c col/codec/avx2.c col/codec/avx512.c col/codec/neon.c col/compress.c \
                               bm25/kernels/kernels.c bm25/kernels/avx2.c bm25/kernels/avx512.c bm25/kernels/neon.c bm25/analyze/analyze.c bm25/analyze/config.c bm25/analyze/stem.c \
                               col/v1/kern/kern.c col/v1/kern/avx2.c col/v1/kern/avx512.c col/v1/kern/neon.c bm25/bitmap/scalar.c bm25/bitmap/avx2.c bm25/bitmap/avx512.c bm25/bitmap/neon.c)
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(CODEC)) $(OUT)/cpushim.o $(OUT)/ccshim.o $(OUT)/pgshim.o
HDR      = $(wildcard $(SRC)/col/codec*.h $(SRC)/col/codec/*.h $(SRC)/col/compress.h $(SRC)/bm25/kernels/kernels*.h $(SRC)/bm25/analyze/analyze*.h $(SRC)/col/v1/kern/kern*.h $(SRC)/col/col.h) ccshim.h

all: $(OUT)/test $(OUT)/bench

$(OUT)/research: research.c research_decode.h research_enc.h research_float.h gen.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ research.c $(LDLIBS)

$(eval $(call common_obj,$(OUT),pgshim))
$(eval $(call common_obj,$(OUT),cpushim))

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: test.c test_kernels.h test_fuzz.h $(SRC)/col/codec/gen.h $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ test.c $(OBJ) $(PGLIB) -lpgport $(LDLIBS)

$(OUT)/bench: bench.c gen.h $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ bench.c $(OBJ) $(PGLIB) -lpgport $(LDLIBS)

research: $(OUT)/research
	$(FLOCK) $(OUT)/research $(ARGS)

test: $(OUT)/test
	$(OUT)/test $(N)

bench: $(OUT)/bench
	$(FLOCK) $(OUT)/bench $(ARGS)

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: all research test bench clean
