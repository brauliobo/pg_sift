# The Vulkan compute engine of pg_sift 6.0, standalone: the device layer (src/agg/vk/), its shaders, and the test and benchmark that run it on a real GPU
# without PostgreSQL. The real column codec of this tree (src/col/codec*) writes the chunks the tests decode. Run from tools/vk-v6.
#   nice make -j4 test            build and run tests/vk_v6_test on the host's GPU        (make test GPU=1 picks a device, see tests/main.c)
#   nice make -j4 bench           build tests/vk_v6_bench
#   make SANITIZE=1 test          ASan + UBSan build into build/san
#   make gpu-shaders              regenerate src/agg/vk/spv.h;  make check   the generated headers are current
#   make test-cost                the cost model of sift.gpu = auto against the measurements it is fitted to (no device)
CC       ?= gcc
HERE     := $(CURDIR)
ROOT     := $(abspath $(HERE)/../..)
WTI      ?= $(ROOT)
OUT      ?= $(HERE)/build/out
ifdef SANITIZE
OUT      := $(HERE)/build/san
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
endif
NICE     ?= nice

WARN      = -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 $(WARN) -DSIFT_VULKAN -I$(ROOT)/src $(SANFLAGS)
LDLIBS    = -ldl -lm -lpthread

VKSRC     = $(wildcard $(ROOT)/src/agg/vk/*.c)
VKOBJ     = $(patsubst $(ROOT)/src/%.c,$(OUT)/%.o,$(VKSRC))
VKHDR     = $(wildcard $(ROOT)/src/agg/vk/*.h) $(ROOT)/src/agg/vk/vk.h $(ROOT)/src/agg/vk/min.h $(ROOT)/src/agg/vk/spv.h

all: $(OUT)/vk_v6_test $(OUT)/vk_v6_bench $(OUT)/vk_v6_cost

$(OUT)/%.o: $(ROOT)/src/%.c $(VKHDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- the real codec, from the integration tree (read only), compiled against the PostgreSQL headers as tools/colcodec does ----
include $(WTI)/src/areas.mk
PGINC    := $(shell pg_config --includedir-server)
PGLIB    := -L$(shell pg_config --libdir)
SRC       = $(WTI)/src
CCFLAGS   = -std=gnu11 -O2 -g $(SIFT_BASE_ISA) -ffp-contract=off -w -I$(SRC) -I$(WTI)/tools/kernels -I$(PGINC) $(SIFT_PGCPP) -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD $(SANFLAGS)
CODEC     = col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec/policy.c col/codec/write.c col/codec/parse.c col/codec/blob.c col/codec/cold.c col/codec/kern.c \
            col/codec/avx2.c col/codec/avx512.c col/codec/neon.c col/compress.c core/par/arena.c bm25/kernels/kernels.c bm25/kernels/avx2.c bm25/kernels/avx512.c bm25/kernels/neon.c \
            bm25/analyze/analyze.c bm25/analyze/config.c bm25/analyze/stem.c col/v1/kern/kern.c col/v1/kern/avx2.c col/v1/kern/avx512.c col/v1/kern/neon.c \
            bm25/bitmap/scalar.c bm25/bitmap/avx2.c bm25/bitmap/avx512.c bm25/bitmap/neon.c
CODECOBJ  = $(patsubst %.c,$(OUT)/codec/%.o,$(CODEC)) $(OUT)/codec/shim.o $(OUT)/codec/ccshim.o $(OUT)/codec/pgshim.o
CODECLIBS = $(PGLIB) -lpgport $(SIFT_PGLIBS) -lzstd -llz4

$(OUT)/codec/%.o: $(SRC)/%.c
	@mkdir -p $(@D)
	$(CC) $(CCFLAGS) -c -o $@ $<
$(OUT)/codec/shim.o: $(WTI)/test/common/cpushim.c
	@mkdir -p $(@D)
	$(CC) $(CCFLAGS) -c -o $@ $<
$(OUT)/codec/ccshim.o: $(WTI)/tools/colcodec/ccshim.c
	@mkdir -p $(@D)
	$(CC) $(CCFLAGS) -I$(WTI)/tools/colcodec -I$(WTI) -c -o $@ $<
$(OUT)/codec/pgshim.o: $(WTI)/test/common/pgshim.c
	@mkdir -p $(@D)
	$(CC) $(CCFLAGS) -c -o $@ $<

# ---- the tests and the benchmark ----
TESTOBJ   = $(OUT)/tests/gen.o $(OUT)/tests/oracle.o $(OUT)/tests/agg.o $(OUT)/tests/finish.o
# T10: the engine's own expression compiler and the translator the server runs (src/agg/gpu/emit.c)
EMITOBJ   = $(OUT)/tests/emit.o $(OUT)/agg/gpu/emit.o $(OUT)/codec/agg/vexpr/compile.o
TCFLAGS   = -std=gnu11 -O2 -g $(WARN) -DSIFT_VULKAN -I$(ROOT)/src -I$(HERE)/tests -I$(SRC) -I$(WTI)/tools/colcodec -I$(PGINC) $(SIFT_PGCPP) $(SIFT_BASE_ISA) -ffp-contract=off $(SANFLAGS)

$(OUT)/tests/%.o: $(HERE)/tests/%.c $(wildcard $(HERE)/tests/*.h) $(VKHDR)
	@mkdir -p $(@D)
	$(CC) $(TCFLAGS) -Wno-sign-compare -Wno-unused-function -c -o $@ $<

$(OUT)/agg/gpu/emit.o: $(ROOT)/src/agg/gpu/emit.c $(ROOT)/src/agg/gpu/emit.h $(ROOT)/src/agg/gpu/v6.h
	@mkdir -p $(@D)
	$(CC) $(TCFLAGS) -Wno-sign-compare -Wno-unused-function -c -o $@ $<

$(OUT)/vk_v6_test: $(OUT)/tests/main.o $(TESTOBJ) $(EMITOBJ) $(VKOBJ) $(CODECOBJ)
	$(CC) $(SANFLAGS) -o $@ $^ $(CODECLIBS) $(LDLIBS)

$(OUT)/tests/bench.o: TCFLAGS += -O3 -mavx2

$(OUT)/vk_v6_bench: $(OUT)/tests/bench.o $(OUT)/tests/bench_probe.o $(TESTOBJ) $(EMITOBJ) $(VKOBJ) $(CODECOBJ)
	$(CC) $(SANFLAGS) -o $@ $^ $(CODECLIBS) $(LDLIBS) -lm

test: $(OUT)/vk_v6_test
	$(OUT)/vk_v6_test $(TESTARGS)

bench: $(OUT)/vk_v6_bench
	$(OUT)/vk_v6_bench $(BENCHARGS)

vkobjs: $(VKOBJ)

gpu-shaders:
	$(MAKE) -C $(ROOT) gpu-shaders

check:
	cd $(ROOT) && python3 scripts/gen-aggvk-min.py --check && scripts/gen-aggvk-spv.sh --check

$(OUT)/vk_v6_cost: $(HERE)/tests/cost.c $(ROOT)/src/agg/gpu/cost.c $(ROOT)/src/agg/gpu/cost.h
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 $(WARN) -I$(ROOT)/src -o $@ $(HERE)/tests/cost.c $(ROOT)/src/agg/gpu/cost.c -lm

test-cost: $(OUT)/vk_v6_cost
	$(OUT)/vk_v6_cost

clean:
	rm -rf $(HERE)/build/out $(HERE)/build/san

.PHONY: all test test-cost bench vkobjs gpu-shaders check clean
