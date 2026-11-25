# The quantized keys of a compact chunk (src/col/v1/gquant.c): the standalone test, against the portable host header (no backend, no SQLite library).
#   make test [N=1]       round trips, bucket bounds, every SIMD level against the scalar decoder on sound and damaged blobs
#   make SANITIZE=1 test  ASan + UBSan build
include ../../test/common/rules.mk
OUT     ?= /tmp/sift-gquant
N       ?= 1
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 $(SIFT_BASE_ISA) -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DSIFT_HOST_SQLITE -I../../sqlite/host $(SIFT_INC)
ifdef SANITIZE
CFLAGS  += $(SAN_ASAN)
OUT     := $(OUT)-san
endif

$(OUT)/test: test.c $(SRC)/col/v1/gquant.c $(SRC)/col/v1/gquant.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ test.c $(SRC)/col/v1/gquant.c

test: $(OUT)/test
	$(OUT)/test $(N)
