# Tests of the portable value code of the wide leaf types; no backend linked.
#   nice make -j2 -C tools/colwide test [N=1]      random and boundary tests (N scales the iterations)
#   make SANITIZE=1 test                            ASan + UBSan build (into $(OUT)-san), what scripts/fuzz.sh colwide runs
include ../../test/common/rules.mk
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 $(SIFT_BASE_ISA) -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare $(SIFT_INC) -I$(PGINC) $(SIFT_PGCPP) -D_GNU_SOURCE
LDLIBS   = $(PGLIBS) -L$(shell $(PG_CONFIG) --pkglibdir) -lm	# the server headers turn printf into pg_printf
OUT     ?= /tmp/sift-colwide
ifdef SANITIZE
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
OUT     := $(OUT)-san
endif
N       ?= 1

$(OUT)/test: test.c $(SRC)/col/v1/wide.c $(SRC)/col/v1/wide.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ test.c $(SRC)/col/v1/wide.c $(LDLIBS)

test: $(OUT)/test
	$(OUT)/test $(N)

clean:
	rm -rf $(OUT) $(OUT)-san

.PHONY: test clean
