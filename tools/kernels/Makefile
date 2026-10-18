# Standalone kernel harness (no backend linked).
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=2]        N million random iterations per kernel family + exhaustive grid
#   make bench             throughput per level (under flock /tmp/sift-perf.lock)
#   make bench-col         the same for the column kernels (src/sift_col_kern*.c)
#   make bench-shred       the shredder kernels (src/sift_shred_kern*.c) against their composition from the shared slots
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-kernels-tools
N       ?= 2

LIB      = $(SRC)/sift_kernels.c $(SRC)/sift_kernels_avx2.c $(SRC)/sift_kernels_avx512.c \
           $(SRC)/sift_col_kern.c $(SRC)/sift_col_kern_avx2.c $(SRC)/sift_col_kern_avx512.c \
           $(SRC)/sift_shred_kern.c $(SRC)/sift_shred_kern_avx2.c $(SRC)/sift_shred_kern_avx512.c \
           $(SRC)/sift_colcodec_kern.c $(SRC)/sift_colcodec_avx2.c $(SRC)/sift_colcodec_avx512.c \
           $(SRC)/sift_aggkern.c $(SRC)/sift_aggkern_avx2.c $(SRC)/sift_aggkern_avx512.c \
           $(SRC)/sift_bitmap_scalar.c $(SRC)/sift_bitmap_avx2.c $(SRC)/sift_bitmap_avx512.c
HDR      = $(SRC)/sift_kernels.h $(SRC)/sift_kernels_int.h $(SRC)/sift_col_kern.h $(SRC)/sift_col_kern_int.h $(SRC)/sift_shred.h $(SRC)/sift_aggkern.h $(SRC)/sift_colcodec.h $(SRC)/sift_colcodec_int.h harness.h

include ../tools.mk

$(OUT)/bench: $(OUT)/bench.o $(OUT)/bench_col.o $(OUT)/bench_shred.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

bench-col: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench col

bench-shred: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench shred

.PHONY: bench-col bench-shred
