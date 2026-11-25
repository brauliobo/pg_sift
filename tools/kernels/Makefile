# Standalone kernel harness (no backend linked).
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=2]        N million random iterations per kernel family + exhaustive grid
#   make bench             throughput per level (under flock /tmp/sift-perf.lock)
#   make bench-col         the same for the column kernels (src/col/v1/kern/kern*.c)
#   make bench-shred       the shredder kernels (src/col/shred/kern*.c) against their composition from the shared slots
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
#   make colodd [R=20]     the column kernels at lengths 0..4097 around every vector width, on exact-size buffers (use SANITIZE=1)
OUT     ?= /tmp/sift-kernels-tools
N       ?= 2

LIB      = $(SRC)/bm25/kernels/kernels.c $(SRC)/bm25/kernels/avx2.c $(SRC)/bm25/kernels/avx512.c $(SRC)/bm25/kernels/neon.c \
           $(SRC)/col/v1/kern/kern.c $(SRC)/col/v1/kern/avx2.c $(SRC)/col/v1/kern/avx512.c $(SRC)/col/v1/kern/neon.c \
           $(SRC)/col/shred/kern.c $(SRC)/col/shred/avx2.c $(SRC)/col/shred/avx512.c $(SRC)/col/shred/neon.c \
           $(SRC)/col/codec/kern.c $(SRC)/col/codec/avx2.c $(SRC)/col/codec/avx512.c $(SRC)/col/codec/neon.c \
           $(SRC)/bm25/bitmap/scalar.c $(SRC)/bm25/bitmap/avx2.c $(SRC)/bm25/bitmap/avx512.c $(SRC)/bm25/bitmap/neon.c
HDR      = $(SRC)/bm25/kernels/kernels.h $(SRC)/bm25/kernels/int.h $(SRC)/col/v1/kern/kern.h $(SRC)/col/v1/kern/int.h $(SRC)/col/shred/shred.h $(SRC)/col/codec/codec.h $(SRC)/col/codec/int.h $(SRC)/col/codec/vec.h harness.h

include ../tools.mk

$(OUT)/bench: $(OUT)/bench.o $(OUT)/bench_col.o $(OUT)/bench_shred.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/colodd: $(OUT)/colodd.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

colodd: $(OUT)/colodd
	$(OUT)/colodd $(R)

bench-col: $(OUT)/bench
	$(FLOCK) $(OUT)/bench col

bench-shred: $(OUT)/bench
	$(FLOCK) $(OUT)/bench shred

.PHONY: bench-col bench-shred colodd
