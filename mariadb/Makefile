# sift for MariaDB: the pg_sift search engine (src/, compiled unmodified except for one thread-local) behind a backend
# shim, as one UDF library.
#   nice make -j4          builds $(OUT)/libsift_udf.so (the MariaDB plugin), $(OUT)/sift_cli (the engine without a server)
#                          and $(OUT)/mdbq, $(OUT)/stress (the statement runner and the client threads of the tests)
# Needs the PostgreSQL server headers and its libpgcommon/libpgport archives (types, macros, UTF-8 tables, qsort and hash
# functions; no backend code is linked) and the MariaDB server headers (MDB_INC: found with mariadb_config / mysql_config,
# else /usr/include/mysql/server). CMakeLists.txt builds the same library.
ROOT    := $(abspath ..)
SRC     := $(ROOT)/src
HERE    := $(abspath .)
OUT     ?= /tmp/sift-mariadb-build
PG_INC  := $(shell pg_config --includedir-server)
PG_LIB  := $(shell pg_config --libdir)
MDB_INC ?= $(shell for c in mariadb_config mysql_config; do d=$$($$c --variable=pkgincludedir 2>/dev/null) && [ -f "$$d/server/mysql_version.h" ] && echo "$$d/server" && exit; done; echo /usr/include/mysql/server)
MDB_CLI ?= $(patsubst %/server,%,$(MDB_INC))
MDB_VER := $(shell sed -n 's/^\#define MYSQL_SERVER_VERSION[[:space:]]*"\(.*\)"/\1/p' $(MDB_INC)/mysql_version.h)
CC      ?= gcc

# the current and top memory context of the engine are per thread: every use becomes a call of the accessors in shim.c
CFLAGS  = -O2 -g -std=gnu11 -fPIC -ffp-contract=off -fwrapv -fno-strict-aliasing -D_GNU_SOURCE -MMD -MP -Wall \
          -Wno-declaration-after-statement -Wno-unused-function -march=x86-64 -mtune=generic -fvisibility=hidden \
          -I$(HERE) -I$(SRC) -I$(PG_INC) -DSIFT_MDB_SERVER_VERSION='"$(MDB_VER)"' \
          -DCurrentMemoryContext='(*sift_cur_cx())' -DTopMemoryContext='(*sift_top_cx())' \
          -Dsift_count=codec_count -Dsift_topk=codec_topk	# the codec's names are those of two UDFs
LIBS   = $(PG_LIB)/libpgcommon_shlib.a $(PG_LIB)/libpgport_shlib.a -lm -lpthread -ldl

# engine sources of src/, compiled unmodified
PGSRC   = sift_bitmap_scalar sift_bitmap_avx2 sift_bitmap_avx512 sift_kernels sift_kernels_avx2 \
          sift_kernels_avx512 sift_analyze sift_analyze_stem sift_query sift_dict sift_posting sift_segment \
          sift_prefix sift_eval
SHIM    = shim store regex engine query feed merge
OBJS    = $(addprefix $(OUT)/,$(addsuffix .o,$(PGSRC) sift_bitmap_tl sift_build_core $(SHIM)))

all: $(OUT)/sift_cli $(OUT)/libsift_udf.so $(OUT)/mdbq $(OUT)/stress

$(OUT)/%.o: $(SRC)/%.c | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(HERE)/%.c $(HERE)/ix.h $(HERE)/engine.h $(HERE)/shim.h | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

# the inverted buffer, runs and merge of src/sift_build.c: the part before the heap build (ambuild)
$(OUT)/sift_build_core.c: $(SRC)/sift_build.c | $(OUT)
	sed '/^\/\* ---------- ambuild ---------- \*\//,$$d' $< >$@
$(OUT)/sift_build_core.o: $(OUT)/sift_build_core.c
	$(CC) $(CFLAGS) -c -o $@ $<

# the seek limit of the bitmap cursors is a global of src/sift_bitmap.c that a query sets while it seeks: per thread here
$(OUT)/sift_bitmap_tl.c: $(SRC)/sift_bitmap.c | $(OUT)
	sed 's/^static uint32_t seek_limit/static __thread uint32_t seek_limit/' $< >$@
$(OUT)/sift_bitmap_tl.o: $(OUT)/sift_bitmap_tl.c
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
	$(CC) -O2 -Wall -I$(MDB_CLI) -o $@ $< -lmariadb

# the client threads of tests/stress.sh
$(OUT)/stress: $(HERE)/tests/stress.c | $(OUT)
	$(CC) -O2 -Wall -pthread -I$(MDB_CLI) -o $@ $< -lmariadb

mdb-inc:
	@echo $(MDB_INC)

-include $(wildcard $(OUT)/*.d)
