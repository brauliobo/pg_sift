# Standalone harness of the aggregation operators (src/agg/kern*.c, no backend linked). The col and bm25 kernels are linked for the
# shim of tools/kernels.
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=20]       N thousand random cases per level (all operators) plus the length grid
#   make bench             ns per 4096-element vector per operator and level (under flock /tmp/sift-perf.lock)
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-aggkern-tools
N       ?= 20

LIB      = $(SRC)/agg/kern.c $(SRC)/agg/kern_avx2.c $(SRC)/agg/kern_avx512.c \
           $(SRC)/col/kern.c $(SRC)/col/kern_avx2.c $(SRC)/col/kern_avx512.c \
           $(SRC)/bm25/kernels.c $(SRC)/bm25/kernels_avx2.c $(SRC)/bm25/kernels_avx512.c \
           $(SRC)/bm25/bitmap_scalar.c $(SRC)/bm25/bitmap_avx2.c $(SRC)/bm25/bitmap_avx512.c
HDR      = $(SRC)/agg/kern.h $(SRC)/agg/kern_int.h $(SRC)/agg/kern_cases.h $(SRC)/col/kern.h $(SRC)/col/kern_int.h harness.h

include ../tools.mk

$(OUT)/bench: $(OUT)/bench.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
