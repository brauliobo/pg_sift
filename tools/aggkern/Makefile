# Standalone harness of the aggregation operators (src/sift_aggkern*.c, no backend linked).
#   nice make -j4          build test + bench into $(OUT)
#   make test [N=20]       N thousand random cases per level (all operators) plus the length grid
#   make bench             ns per 4096-element vector per operator and level (under flock /tmp/sift-perf.lock)
#   make SANITIZE=1 test   ASan + UBSan build (into $(OUT)-san)
CC      ?= gcc
SRC      = ../../src
# Same code generation flags as the extension (Makefile PG_CFLAGS)
CFLAGS   = -std=gnu11 -O2 -g -march=x86-64 -mtune=generic -ffp-contract=off -Wall -Wextra \
           -Wno-unused-parameter -Wno-sign-compare -I$(SRC) -I$(shell pg_config --includedir-server)
LDLIBS   = -lm -lpthread
OUT     ?= /tmp/sift-aggkern-tools
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OUT     := $(OUT)-san
endif
LIB      = $(SRC)/sift_aggkern.c $(SRC)/sift_aggkern_avx2.c $(SRC)/sift_aggkern_avx512.c
HDR      = $(SRC)/sift_aggkern.h $(SRC)/sift_aggkern_int.h $(SRC)/sift_aggkern_cases.h harness.h
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(LIB)) $(OUT)/shim.o
N       ?= 20

all: $(OUT)/test $(OUT)/bench

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: $(OUT)/test.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/bench: $(OUT)/bench.o $(OUT)/alt.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(OUT)/test
	flock /tmp/sift-perf.lock $(OUT)/test $(N)

bench: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: all test bench clean
