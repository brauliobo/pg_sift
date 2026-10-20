# The fuzz targets of scripts/fuzz.sh (colchunk, bm25, shred; test/fuzz/README.md). No backend is linked: tools/pgshim.c and fz_shim.c stand in for it.
#   nice make -j4 -C test/fuzz OUT=/tmp/x all         build the three targets
#   make FZ_DRIVER=1 ...                              the portable coverage-guided driver (fz_driver.c) instead of libFuzzer
#
# With clang (and its libFuzzer runtime) the targets are libFuzzer binaries; with gcc, or FZ_DRIVER=1, fz_driver.c is main() and the
# edges come from -fsanitize-coverage=trace-pc. Both take the libFuzzer command line (-max_total_time=SEC, -runs=N, a corpus
# directory), so scripts/fuzz.sh does not care. Always ASan + UBSan, any report aborts (-fno-sanitize-recover=all). No -fwrapv: signed
# overflow is a finding here, where the extension itself is built -fwrapv like the server.
OUT     ?= /tmp/sift-fuzz-build
SRC      = ../../src
include $(SRC)/areas.mk
KT       = ../../tools/kernels
PGINC   := $(shell pg_config --includedir-server)
PGLIB   := -L$(shell pg_config --libdir) -L$(shell pg_config --pkglibdir)

ifndef FZ_DRIVER
FZ_CC   := $(shell command -v clang 2>/dev/null)
ifneq ($(FZ_CC),)
ifeq ($(shell echo 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n) { return 0; }' | $(FZ_CC) -fsanitize=fuzzer -x c - -o /dev/null 2>/dev/null && echo ok),ok)
FZ_LIBFUZZER = 1
endif
endif
endif
ifdef FZ_LIBFUZZER
CC       = $(FZ_CC)
COV      = -fsanitize=fuzzer-no-link
LINKFZ   = -fsanitize=fuzzer
else
CC      ?= gcc
COV      = -fsanitize-coverage=trace-pc
endif

# SAN, DSAN (the driver's) and DEFS/COLZ can be replaced: MSan needs SAN='-fsanitize=memory -fsanitize-memory-track-origins' DEFS= COLZ= (lz4 and zstd are
# not instrumented), valgrind needs SAN='-fno-omit-frame-pointer' DSAN= COV= FZ_DRIVER=1 (docs/SAFETY.md)
SAN     ?= -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer
DSAN    ?= -fsanitize=address,undefined
DEFS    ?= -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD
COLZ    ?= -lzstd -llz4
CFLAGS  += -std=gnu11 -O1 -g $(SAN) -march=x86-64 -mtune=generic -ffp-contract=off -Wall -Wno-unused-function -Wno-unused-parameter \
           -Wno-sign-compare $(SIFT_INC) -I$(KT) -I. -I$(PGINC) $(DEFS) -D_GNU_SOURCE
LDLIBS   = -lm $(COLZ) $(PGLIB) -lpgport -lpgcommon

# every source the three targets decode with (the portable core, SRC/portable-col.list and portable.list members), and the stand-ins
CORE     = sift_colcodec sift_colcodec_kern sift_colcodec_avx2 sift_colcodec_avx512 sift_colz sift_col_kern sift_col_kern_avx2 \
           sift_col_kern_avx512 sift_col_enc sift_shred sift_shred_kern sift_shred_kern_avx2 sift_shred_kern_avx512 \
           bm25/kernels bm25/kernels_avx2 bm25/kernels_avx512 bm25/analyze bm25/analyze_config bm25/analyze_stem bm25/posting bm25/bitmap bm25/blob bm25/topk bm25/bitmap_scalar \
           bm25/bitmap_avx2 bm25/bitmap_avx512 sift_span
OBJ      = $(addprefix $(OUT)/obj/,$(addsuffix .o,$(CORE))) $(OUT)/shim.o $(OUT)/pgshim.o $(OUT)/fz_shim.o
HDR      = $(wildcard $(SRC)/*/*.h) fz_shim.h ../../tools/pgshim.h Makefile
ifndef FZ_LIBFUZZER
DRIVER   = $(OUT)/fz_driver.o
endif

all: $(addprefix $(OUT)/,colchunk bm25 shred)

$(OUT)/obj/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(COV) -c -o $@ $<

$(OUT)/obj/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(COV) -c -o $@ $<

# the cpu stand-in of tools/kernels; its stub of sift_k would clash with the real one of bm25/bitmap.c
$(OUT)/shim.o: $(KT)/shim.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(COV) -Dsift_k=fz_unused_sift_k -c -o $@ $<

# the stand-ins are not instrumented
$(OUT)/pgshim.o: ../../tools/pgshim.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/fz_shim.o: fz_shim.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/fz_%.o: fz_%.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(COV) -c -o $@ $<

# the driver is not instrumented: it is the callback
$(OUT)/fz_driver.o: fz_driver.c
	@mkdir -p $(OUT)
	$(CC) -O1 -g $(DSAN) -c -o $@ $<

$(OUT)/colchunk: $(OUT)/fz_colchunk.o $(OBJ) $(DRIVER)
	$(CC) $(CFLAGS) $(LINKFZ) -o $@ $^ $(LDLIBS)

$(OUT)/bm25: $(OUT)/fz_bm25.o $(OBJ) $(DRIVER)
	$(CC) $(CFLAGS) $(LINKFZ) -o $@ $^ $(LDLIBS)

$(OUT)/shred: $(OUT)/fz_shred.o $(OBJ) $(DRIVER)
	$(CC) $(CFLAGS) $(LINKFZ) -o $@ $^ $(LDLIBS)

# every file of regress/ is an input that crashed a target before its fix, named TARGET-what: replays them, fails if one comes back
regress: all
	@for f in regress/*; do t=$${f#regress/}; $(OUT)/$${t%%-*} $$f >/dev/null 2>&1 || { echo "FAIL $$f"; exit 1; }; done; echo "regress: ok"

clean:
	rm -rf $(OUT)

.PHONY: all clean regress
