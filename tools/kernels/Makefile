# Standalone kernel harness (no backend linked).
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=2]        N million random iterations per kernel family + exhaustive grid
#   make bench             throughput per level (under flock /tmp/sift-perf.lock)
#   make bench-col         the same for the column kernels (src/col/kern*.c)
#   make bench-shred       the shredder kernels (src/col/shred_kern*.c) against their composition from the shared slots
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-kernels-tools
N       ?= 2

LIB      = $(SRC)/bm25/kernels.c $(SRC)/bm25/kernels_avx2.c $(SRC)/bm25/kernels_avx512.c \
           $(SRC)/col/kern.c $(SRC)/col/kern_avx2.c $(SRC)/col/kern_avx512.c \
           $(SRC)/col/shred_kern.c $(SRC)/col/shred_kern_avx2.c $(SRC)/col/shred_kern_avx512.c \
           $(SRC)/col/codec_kern.c $(SRC)/col/codec_avx2.c $(SRC)/col/codec_avx512.c \
           $(SRC)/agg/sift_aggkern.c $(SRC)/agg/sift_aggkern_avx2.c $(SRC)/agg/sift_aggkern_avx512.c \
           $(SRC)/bm25/bitmap_scalar.c $(SRC)/bm25/bitmap_avx2.c $(SRC)/bm25/bitmap_avx512.c
HDR      = $(SRC)/bm25/kernels.h $(SRC)/bm25/kernels_int.h $(SRC)/col/kern.h $(SRC)/col/kern_int.h $(SRC)/col/shred.h $(SRC)/agg/sift_aggkern.h $(SRC)/col/codec.h $(SRC)/col/codec_int.h harness.h

include ../tools.mk

$(OUT)/bench: $(OUT)/bench.o $(OUT)/bench_col.o $(OUT)/bench_shred.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

bench-col: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench col

bench-shred: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench shred

.PHONY: bench-col bench-shred
