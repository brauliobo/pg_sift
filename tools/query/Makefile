# Standalone ASan/UBSan harness for the SiftQL module:
#   nice make -C tools/query
#   python3 tools/query/gen.py -n 100000 -o /tmp/q.copy && tools/query/harness /tmp/q.copy
SRC = ../../src
include $(SRC)/areas.mk
PG_INC := $(shell pg_config --includedir-server)
PG_LIB := -L$(shell pg_config --libdir) -L$(shell pg_config --pkglibdir)
CFLAGS = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
	-ffp-contract=off -fwrapv -fno-strict-aliasing -Wall $(SIFT_INC) -I$(PG_INC) $(SIFT_PGCPP) -D_GNU_SOURCE

QUERY_C = $(SRC)/bm25/query/query.c $(SRC)/bm25/query/analyze.c $(SRC)/bm25/query/match.c

harness: harness.c ../pgshim.c ../pgshim.h $(QUERY_C) $(SRC)/bm25/query/query.h $(SRC)/bm25/query/int.h
	$(CC) $(CFLAGS) -o $@ harness.c ../pgshim.c $(QUERY_C) $(PG_LIB) -lpgcommon -lpgport $(SIFT_PGLIBS) -lm

clean:
	rm -f harness

.PHONY: clean
