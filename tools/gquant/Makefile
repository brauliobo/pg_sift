# The quantized keys of a compact chunk (src/col/gquant.c): the standalone test, against the portable host header (no backend, no SQLite library).
#   make test [N=1]       round trips, bucket bounds, every SIMD level against the scalar decoder on sound and damaged blobs
#   make SANITIZE=1 test  ASan + UBSan build
CC      ?= gcc
SRC      = ../../src
include $(SRC)/areas.mk
OUT     ?= /tmp/sift-gquant
N       ?= 1
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 $(SIFT_BASE_ISA) -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DSIFT_HOST_SQLITE -I../../sqlite/host $(SIFT_INC)
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OUT     := $(OUT)-san
endif

$(OUT)/test: test.c $(SRC)/col/gquant.c $(SRC)/col/gquant.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ test.c $(SRC)/col/gquant.c

test: $(OUT)/test
	$(OUT)/test $(N)
