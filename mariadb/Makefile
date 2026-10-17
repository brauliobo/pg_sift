# sift for MariaDB: the pg_sift search engine (src/, compiled unmodified) behind a backend shim, as one UDF library.
#   nice make -j4          builds $(OUT)/libsift_udf.so (the MariaDB plugin), $(OUT)/sift_cli (the engine without a server)
#                          and $(OUT)/mdbq (the statement runner of the tests)
# Needs the PostgreSQL server headers and its libpgcommon/libpgport archives (types, macros, UTF-8 tables, qsort and hash
# functions; no backend code is linked) and the MariaDB server headers.
ROOT    := $(abspath ..)
SRC     := $(ROOT)/src
HERE    := $(abspath .)
OUT     ?= /tmp/sift-mariadb-build
PG_INC  := $(shell pg_config --includedir-server)
PG_LIB  := $(shell pg_config --libdir)
MDB_INC ?= /usr/include/mysql/server
CC      ?= gcc

CFLAGS  = -O2 -g -std=gnu11 -fPIC -ffp-contract=off -fwrapv -fno-strict-aliasing -D_GNU_SOURCE -MMD -MP -Wall \
          -Wno-declaration-after-statement -Wno-unused-function -march=x86-64 -mtune=generic -fvisibility=hidden \
          -I$(HERE)/shim -I$(SRC) -I$(PG_INC) \
          -Dsift_count=codec_count -Dsift_topk=codec_topk	# the codec's names are those of two UDFs
LIBS   = $(PG_LIB)/libpgcommon_shlib.a $(PG_LIB)/libpgport_shlib.a -lm -lpthread

# engine sources of src/, compiled unmodified
PGSRC   = sift_bitmap sift_bitmap_scalar sift_bitmap_avx2 sift_bitmap_avx512 sift_kernels sift_kernels_avx2 \
          sift_kernels_avx512 sift_analyze sift_analyze_stem sift_query sift_dict sift_posting sift_segment \
          sift_prefix sift_eval
SHIM    = shim store engine
OBJS    = $(addprefix $(OUT)/,$(addsuffix .o,$(PGSRC) sift_build_core $(SHIM)))

all: $(OUT)/sift_cli $(OUT)/libsift_udf.so $(OUT)/mdbq

$(OUT)/%.o: $(SRC)/%.c | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(HERE)/%.c | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

# the inverted buffer, runs and merge of src/sift_build.c: the part before the heap build (ambuild)
$(OUT)/sift_build_core.c: $(SRC)/sift_build.c | $(OUT)
	sed '/^\/\* ---------- ambuild ---------- \*\//,$$d' $< >$@
$(OUT)/sift_build_core.o: $(OUT)/sift_build_core.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT):
	mkdir -p $@

$(OUT)/sift_cli: $(OUT)/cli.o $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

# the UDFs include MariaDB headers and nothing of PostgreSQL; the engine is linked in with its symbols hidden
$(OUT)/udf.o: $(HERE)/udf.c $(HERE)/engine.h | $(OUT)
	$(CC) -O2 -g -fPIC -fvisibility=hidden -Wall -MMD -MP -I$(MDB_INC) -I$(MDB_INC)/private -c -o $@ $<

$(OUT)/libsift_udf.so: $(OUT)/udf.o $(OBJS)
	$(CC) -shared -o $@ $^ $(LIBS) -Wl,--exclude-libs,ALL -Wl,-Bsymbolic

# the statement runner of the tests and the benchmark (libmariadb client)
$(OUT)/mdbq: $(HERE)/bench/mdbq.c | $(OUT)
	$(CC) -O2 -Wall -I/usr/include/mysql -o $@ $< -lmariadb

-include $(wildcard $(OUT)/*.d)
