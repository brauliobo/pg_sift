# Standalone harness of the 2048-row operators (src/agg/kern6*.c, no backend linked).
#   nice make -j2 test [N=20]   N thousand random cases per level plus the length grid
#   make SANITIZE=1 test        ASan + UBSan build (into $(OUT)-san)
OUT     ?= /tmp/sift-aggkern6-tools
N       ?= 20

LIB      = $(addprefix $(SRC)/agg/,kern6.c kern6_cmp.c kern6_avx2.c kern6_avx2_cmp.c kern6_avx2_agg.c kern6_avx512.c kern6_avx512_cmp.c kern6_avx512_agg.c kern6_neon.c kern6_neon_cmp.c kern6_neon_agg.c kern6_sbbf.c) $(SRC)/core/sbbf.c
HDR      = $(wildcard $(SRC)/agg/kern6*.h) $(SRC)/agg/vec.h $(wildcard bench.h cases_*.h test_*.h)

CC      ?= gcc
SRC      = ../../src
include $(SRC)/areas.mk
CFLAGS   = -std=gnu11 -O2 -g $(SIFT_BASE_ISA) -ffp-contract=off -Wall -Wextra \
           -Wno-unused-parameter -Wno-sign-compare $(SIFT_FMV_CPPFLAGS) $(SIFT_INC) -I$(shell pg_config --includedir-server) $(SIFT_PGCPP)
LDLIBS   = -lm -pthread
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
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
