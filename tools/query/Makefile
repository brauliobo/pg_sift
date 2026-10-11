# Standalone ASan/UBSan harness for the SiftQL module:
#   nice make -C tools/query
#   python3 tools/query/gen.py -n 100000 -o /tmp/q.copy && tools/query/harness /tmp/q.copy
SRC = ../../src
PG_INC := $(shell pg_config --includedir-server)
PG_LIB := $(shell pg_config --libdir)
CFLAGS = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
	-ffp-contract=off -fwrapv -fno-strict-aliasing -Wall -I$(SRC) -I$(PG_INC) -D_GNU_SOURCE

harness: harness.c $(SRC)/sift_query.c $(SRC)/sift_query.h
	$(CC) $(CFLAGS) -o $@ harness.c $(SRC)/sift_query.c -L$(PG_LIB) -lpgcommon -lpgport -lm

clean:
	rm -f harness

.PHONY: clean
