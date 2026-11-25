# Standalone harness of the 2048-row operators (src/agg/kern/kern6*.c, no backend linked).
#   nice make -j2 test [N=20]   N thousand random cases per level plus the length grid
#   make SANITIZE=1 test        ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-aggkern6-tools
N       ?= 20

LIB      = $(addprefix $(SRC)/agg/,kern6/kern6.c kern6/hash.c kern6/cmp.c kern6/avx2.c kern6/avx2_cmp.c kern6/avx2_agg.c kern6/avx512.c kern6/avx512_cmp.c kern6/avx512_agg.c kern6/neon.c kern6/neon_cmp.c kern6/neon_agg.c kern6/sbbf.c) $(SRC)/core/sbbf.c
HDR      = $(wildcard $(SRC)/agg/kern/kern6*.h) $(SRC)/agg/vec.h $(wildcard bench.h cases_*.h test_*.h)

include ../../test/common/rules.mk
CFLAGS   = -std=gnu11 -O2 -g $(SIFT_BASE_ISA) -ffp-contract=off -Wall -Wextra \
           -Wno-unused-parameter -Wno-sign-compare $(SIFT_FMV_CPPFLAGS) $(SIFT_INC) $(SIFT_TEST_INC) -I$(PGINC) $(SIFT_PGCPP)
LDLIBS   = -lm -pthread
ifdef SANITIZE
CFLAGS  += $(SAN_ASAN)
OUT     := $(OUT)-san
endif
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/%.o,$(LIB))

$(OUT)/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test.o: test.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: $(OUT)/test.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(OUT)/test
	$(OUT)/test $(N)

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: test clean
