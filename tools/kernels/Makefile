# Standalone kernel harness (no backend linked).
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=2]        N million random iterations per kernel family + exhaustive grid
#   make bench             throughput per level (under flock /tmp/sift-perf.lock)
#   make bench-col         the same for the column kernels (src/sift_col_kern*.c)
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
CC      ?= gcc
SRC      = ../../src
# Same code generation flags as the extension (Makefile PG_CFLAGS)
CFLAGS   = -std=gnu11 -O2 -g -march=x86-64 -mtune=generic -ffp-contract=off -Wall -Wextra \
           -Wno-unused-parameter -Wno-sign-compare -I$(SRC) -I$(shell pg_config --includedir-server)
LDLIBS   = -lm -lpthread
OUT     ?= /tmp/sift-kernels-tools
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OUT     := $(OUT)-san
endif
LIB      = $(SRC)/sift_kernels.c $(SRC)/sift_kernels_avx2.c $(SRC)/sift_kernels_avx512.c \
           $(SRC)/sift_col_kern.c $(SRC)/sift_col_kern_avx2.c $(SRC)/sift_col_kern_avx512.c \
           $(SRC)/sift_bitmap_scalar.c $(SRC)/sift_bitmap_avx2.c $(SRC)/sift_bitmap_avx512.c
HDR      = $(SRC)/sift_kernels.h $(SRC)/sift_kernels_int.h $(SRC)/sift_col_kern.h $(SRC)/sift_col_kern_int.h harness.h
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(LIB)) $(OUT)/shim.o
N       ?= 2

all: $(OUT)/test $(OUT)/bench

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: $(OUT)/test.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/bench: $(OUT)/bench.o $(OUT)/bench_col.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(OUT)/test
	flock /tmp/sift-perf.lock $(OUT)/test $(N)

bench: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench

bench-col: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench col

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: all test bench bench-col clean
