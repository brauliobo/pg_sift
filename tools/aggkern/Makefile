# Standalone harness of the aggregation operators (src/sift_aggkern*.c, no backend linked). The col kernels are linked for the
# position table the AVX2 count_changes shares with bits_to_idx.
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=20]       N thousand random cases per level (all operators) plus the length grid
#   make bench             ns per 4096-element vector per operator and level (under flock /tmp/sift-perf.lock)
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-aggkern-tools
N       ?= 20

LIB      = $(SRC)/sift_aggkern.c $(SRC)/sift_aggkern_avx2.c $(SRC)/sift_aggkern_avx512.c \
           $(SRC)/sift_col_kern.c $(SRC)/sift_col_kern_avx2.c $(SRC)/sift_col_kern_avx512.c \
           $(SRC)/sift_kernels.c $(SRC)/sift_kernels_avx2.c $(SRC)/sift_kernels_avx512.c \
           $(SRC)/sift_bitmap_scalar.c $(SRC)/sift_bitmap_avx2.c $(SRC)/sift_bitmap_avx512.c
HDR      = $(SRC)/sift_aggkern.h $(SRC)/sift_aggkern_int.h $(SRC)/sift_aggkern_cases.h $(SRC)/sift_col_kern.h $(SRC)/sift_col_kern_int.h harness.h

include ../tools.mk

$(OUT)/bench: $(OUT)/bench.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
