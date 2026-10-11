# Standalone codec harness.  Build: nice make -j4; tests: make test; data: make data
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -ffp-contract=off -Wall -Wextra -Wno-unused-function
SRC      = ../../src
LIB      = $(SRC)/sift_bitmap.c $(SRC)/sift_bitmap_scalar.c $(SRC)/sift_bitmap_avx2.c $(SRC)/sift_bitmap_avx512.c
HDR      = $(SRC)/sift_bitmap.h $(SRC)/sift_bitmap_kern.h
OUT     ?= /tmp/sift-codec
OBJ      = $(patsubst $(SRC)/%.c,$(OUT)/obj/%.o,$(LIB))

all: $(OUT)/test $(OUT)/bench $(OUT)/corpus $(OUT)/stats

$(OUT)/obj/%.o: $(SRC)/%.c $(HDR)
	@mkdir -p $(OUT)/obj
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test: test.c $(OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ test.c $(OBJ) -lm

# optional CRoaring amalgamation (roaring.c, roaring.h) for comparisons
ROARING ?= $(OUT)/croaring
ifneq ($(wildcard $(ROARING)/roaring.c),)
ROAR_CFLAGS = -DHAVE_ROARING -I$(ROARING)
ROAR_OBJ    = $(OUT)/obj/roaring.o
$(ROAR_OBJ): $(ROARING)/roaring.c
	@mkdir -p $(OUT)/obj
	$(CC) -O2 -g -c -o $@ $<
endif

$(OUT)/bench: bench.c data.h baselines.h $(OBJ) $(ROAR_OBJ) $(HDR)
	$(CC) $(CFLAGS) $(ROAR_CFLAGS) -o $@ bench.c $(OBJ) $(ROAR_OBJ) -lm

$(OUT)/corpus: corpus.c
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/stats: stats.c data.h
	$(CC) $(CFLAGS) -o $@ $<

test: $(OUT)/test
	$(OUT)/test

# reviews.tsv: ctid + title + comments text of the source documents table (read-only COPY)
data:
	@mkdir -p $(OUT)
	psql source_db -c "\copy (select (ctid::text::point)[0]::bigint, (ctid::text::point)[1]::int, regexp_replace(coalesce(title,'') || ' ' || coalesce((select string_agg(c->>'comment',' ') from jsonb_array_elements(case when jsonb_typeof(comments)='array' then comments else '[]' end) c),''), '[[:space:]\\\\]+', ' ', 'g') from reviews) to '$(OUT)/reviews.tsv'"
	$(OUT)/corpus $(OUT)/reviews.tsv $(OUT)

clean:
	rm -rf $(OUT)/obj $(OUT)/test $(OUT)/bench $(OUT)/corpus $(OUT)/stats

.PHONY: all test data clean
