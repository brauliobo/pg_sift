# SQLite loadable extension around the portable pg_sift sources.
#   nice make -j4          builds build/sift.so (load with .load build/sift)
#   make test              ../sqlite/test.sh
# The sources of ../src listed in PORTABLE are compiled unmodified, against the PostgreSQL API shim in shim/.
SRC := ../src
B := build
CC ?= cc
OPT ?= -O2
# SIFT_ISA: the x86-64 baseline, SIMD kernels opt in per function (see src/sift_cpu.h); -ffp-contract=off keeps scores bit-identical
ifeq ($(shell uname -m),x86_64)
ISA := -march=x86-64 -mtune=generic
endif
CFLAGS += $(OPT) -g -ffp-contract=off -fwrapv -fno-strict-aliasing -fPIC -Wall -Wno-misleading-indentation \
	-Wno-unused-function -Wno-declaration-after-statement -MMD -D_GNU_SOURCE $(ISA) -Ishim -I$(SRC)

PORTABLE := sift_bitmap sift_bitmap_scalar sift_bitmap_avx2 sift_bitmap_avx512 sift_kernels sift_kernels_avx2 \
	sift_kernels_avx512 sift_analyze sift_analyze_stem sift_query sift_posting sift_dict sift_segment sift_prefix \
	sift_filter sift_eval sift_cpu
OWN := shim shim_regex sqstore sqstubs sqbuild sqvtab
OBJS := $(addprefix $(B)/,$(addsuffix .o,$(PORTABLE) $(OWN)))

all: $(B)/sift.so

$(B)/sift.so: $(OBJS)
	$(CC) -shared -o $@ $(OBJS) -lm

$(B)/%.o: $(SRC)/%.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B):
	mkdir -p $(B)

check test: all
	./test.sh

clean:
	rm -rf $(B)

-include $(wildcard $(B)/*.d)

.PHONY: all check test clean
