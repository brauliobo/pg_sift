# Standalone kernel harness (no backend linked).
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=2]        N million random iterations per kernel family + exhaustive grid
#   make bench             throughput per level (under flock /tmp/sift-perf.lock)
#   make bench-col         the same for the column kernels (src/col/sift_col_kern*.c)
#   make bench-shred       the shredder kernels (src/col/sift_shred_kern*.c) against their composition from the shared slots
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-kernels-tools
N       ?= 2

LIB      = $(SRC)/bm25/sift_kernels.c $(SRC)/bm25/sift_kernels_avx2.c $(SRC)/bm25/sift_kernels_avx512.c \
           $(SRC)/col/sift_col_kern.c $(SRC)/col/sift_col_kern_avx2.c $(SRC)/col/sift_col_kern_avx512.c \
           $(SRC)/col/sift_shred_kern.c $(SRC)/col/sift_shred_kern_avx2.c $(SRC)/col/sift_shred_kern_avx512.c \
           $(SRC)/col/sift_colcodec_kern.c $(SRC)/col/sift_colcodec_avx2.c $(SRC)/col/sift_colcodec_avx512.c \
           $(SRC)/agg/sift_aggkern.c $(SRC)/agg/sift_aggkern_avx2.c $(SRC)/agg/sift_aggkern_avx512.c \
           $(SRC)/bm25/sift_bitmap_scalar.c $(SRC)/bm25/sift_bitmap_avx2.c $(SRC)/bm25/sift_bitmap_avx512.c
HDR      = $(SRC)/bm25/sift_kernels.h $(SRC)/bm25/sift_kernels_int.h $(SRC)/col/sift_col_kern.h $(SRC)/col/sift_col_kern_int.h $(SRC)/col/sift_shred.h $(SRC)/agg/sift_aggkern.h $(SRC)/col/sift_colcodec.h $(SRC)/col/sift_colcodec_int.h harness.h

include ../tools.mk

$(OUT)/bench: $(OUT)/bench.o $(OUT)/bench_col.o $(OUT)/bench_shred.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

bench-col: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench col

bench-shred: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench shred

.PHONY: bench-col bench-shred
