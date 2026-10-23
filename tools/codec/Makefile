# Standalone codec harness.  Build: nice make -j4; tests: make test; data: make data
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -ffp-contract=off -Wall -Wextra -Wno-unused-function $(SIFT_INC)
SRC      = ../../src
include $(SRC)/areas.mk
LIB      = $(SRC)/bm25/bitmap.c $(SRC)/bm25/blob.c $(SRC)/bm25/topk.c $(SRC)/bm25/bitmap_scalar.c $(SRC)/bm25/bitmap_avx2.c $(SRC)/bm25/bitmap_avx512.c $(SRC)/bm25/bitmap_neon.c
HDR      = $(SRC)/bm25/bitmap.h $(SRC)/bm25/bitmap_int.h $(SRC)/bm25/bitmap_kern.h
OUT     ?= /tmp/sift-codec
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/obj/%.o,$(LIB)) $(OUT)/obj/stub.o

all: $(OUT)/test $(OUT)/bench $(OUT)/corpus $(OUT)/stats

$(OUT)/obj/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/obj/stub.o: stub.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SIFT_INC) -c -o $@ $<

$(OUT)/test: test.c $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ test.c $(OBJ) -lm

# optional CRoaring amalgamation (roaring.c, roaring.h) for comparisons
ROARING ?= $(OUT)/croaring
ifneq ($(wildcard $(ROARING)/roaring.c),)
ROAR_CFLAGS = -DHAVE_ROARING -I$(ROARING)
ROAR_OBJ    = $(OUT)/obj/roaring.o
$(ROAR_OBJ): $(ROARING)/roaring.c
	@mkdir -p $(@D)
	$(CC) -O2 -g -c -o $@ $<
endif

$(OUT)/bench: bench.c data.h baselines.h $(OBJ) $(ROAR_OBJ) $(HDR)
	$(CC) $(CFLAGS) $(ROAR_CFLAGS) -o $@ bench.c $(OBJ) $(ROAR_OBJ) -lm

$(OUT)/fuzz: fuzz.c $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ fuzz.c $(OBJ) -lm

$(OUT)/corpus: corpus.c
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/stats: stats.c data.h
	$(CC) $(CFLAGS) -o $@ $<

test: $(OUT)/test
	$(OUT)/test

# corrupted-blob fuzzer; run with CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all'
fuzz: $(OUT)/fuzz
	$(OUT)/fuzz $(FUZZARGS)

# articles.tsv: synthetic ctids (five documents per page) and the text of the Simple English Wikipedia
# articles (bench/datasets/fetch.sh articles); ARTICLES=path to bench's articles.tsv
ARTICLES ?= /tmp/sift-bench/data/articles.tsv
data:
	@mkdir -p $(OUT)
	awk -F'\t' '{ printf "%d\t%d\t%s\n", int((NR - 1) / 5), (NR - 1) % 5 + 1, $$2 }' $(ARTICLES) >$(OUT)/articles.tsv
	$(OUT)/corpus $(OUT)/articles.tsv $(OUT)

clean:
	rm -rf $(OUT)/obj $(OUT)/test $(OUT)/fuzz $(OUT)/bench $(OUT)/corpus $(OUT)/stats

.PHONY: all test fuzz data clean
