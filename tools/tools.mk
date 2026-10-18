# The part of the Makefiles of tools/kernels and tools/aggkern (standalone harnesses, no backend linked) that they share.
# The includer sets, before including it: OUT (default), N (default), LIB (the src/ sources linked), HDR (extra header dependencies)
# and, after it, the bench target and what else it builds. test and bench run under flock /tmp/sift-perf.lock.
CC      ?= gcc
SRC      = ../../src
KT       = ../kernels
# Same code generation flags as the extension (Makefile PG_CFLAGS)
CFLAGS   = -std=gnu11 -O2 -g -march=x86-64 -mtune=generic -ffp-contract=off -Wall -Wextra \
           -Wno-unused-parameter -Wno-sign-compare -I$(SRC) -I$(shell pg_config --includedir-server)
LDLIBS   = -lm -lpthread
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OUT     := $(OUT)-san
endif
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(LIB)) $(OUT)/shim.o

all: $(OUT)/test $(OUT)/bench

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/shim.o: $(KT)/shim.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: $(OUT)/test.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(OUT)/test
	flock /tmp/sift-perf.lock $(OUT)/test $(N)

bench: $(OUT)/bench
	flock /tmp/sift-perf.lock $(OUT)/bench

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: all test bench clean
