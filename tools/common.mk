# The part of the Makefiles of tools/kernels and tools/aggkern (standalone harnesses, no backend linked) that they share, on test/common/rules.mk.
# The includer sets, before including it: OUT (default), N (default), LIB (the src/ sources linked), HDR (extra header dependencies)
# and, after it, the bench target and what else it builds. test and bench run under flock /tmp/sift-perf.lock.
include ../../test/common/rules.mk
KT       = ../kernels
# Same code generation flags as the extension (Makefile PG_CFLAGS)
CFLAGS   = -std=gnu11 -O2 -g $(SIFT_BASE_ISA) -ffp-contract=off -Wall -Wextra \
           -Wno-unused-parameter -Wno-sign-compare $(SIFT_INC) -I$(PGINC) $(SIFT_PGCPP)
LDLIBS   = -lm -lpthread
ifdef SANITIZE
CFLAGS  += $(SAN_ASAN)
OUT     := $(OUT)-san
endif
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(LIB)) $(OUT)/cpushim.o

all: $(OUT)/test $(OUT)/bench

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(eval $(call common_obj,$(OUT),cpushim))

$(OUT)/test: $(OUT)/test.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(OUT)/test
	$(FLOCK) $(OUT)/test $(N)

bench: $(OUT)/bench
	$(FLOCK) $(OUT)/bench

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: all test bench clean
