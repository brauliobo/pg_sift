# sift for MariaDB: the portable core of pg_sift (the files of src/portable.list, compiled unmodified) against the host in
# host/, as one UDF library.
#   nice make -j4          builds $(OUT)/libsift_udf.so (the MariaDB plugin), $(OUT)/sift_cli (the engine without a server)
#                          and $(OUT)/mdbq, $(OUT)/stress (the statement runner and the client threads of the tests)
# Needs the MariaDB server headers (MDB_INC: found with mariadb_config / mysql_config, else /usr/include/mysql/server) and
# nothing else: the host vocabulary is the C library (host/host.h includes the one of sqlite/host). CMakeLists.txt builds the
# same library.
ROOT    := $(abspath ..)
SRC     := $(ROOT)/src
include $(SRC)/areas.mk
SQHOST  := $(ROOT)/sqlite/host
HERE    := $(abspath .)
OUT     ?= /tmp/sift-mariadb-build
MDB_INC ?= $(shell for c in mariadb_config mysql_config; do d=$$($$c --variable=pkgincludedir 2>/dev/null) && [ -f "$$d/server/mysql_version.h" ] && echo "$$d/server" && exit; done; echo /usr/include/mysql/server)
MDB_CLI ?= $(patsubst %/server,%,$(MDB_INC))
# The release version: the repository tag without the "v" (scripts/package-mariadb.sh checks that VERSION matches the tag)
SIFT_VERSION ?= $(shell cat $(HERE)/VERSION)
MDB_VER := $(shell sed -n 's/^\#define MYSQL_SERVER_VERSION[[:space:]]*"\(.*\)"/\1/p' $(MDB_INC)/mysql_version.h)
CC      ?= gcc

# -Dsift_count/-Dsift_topk: the codec's names are those of two UDFs
CFLAGS  = -O2 -g -std=gnu11 -fPIC -ffp-contract=off -fwrapv -fno-strict-aliasing -D_GNU_SOURCE -MMD -MP -Wall \
          -Wno-declaration-after-statement -Wno-unused-function -Wno-misleading-indentation -march=x86-64 -mtune=generic \
          -fvisibility=hidden -DSIFT_HOST_MARIADB -I$(HERE)/host -I$(HERE) $(SIFT_INC) -DSIFT_MDB_SERVER_VERSION='"$(MDB_VER)"' \
          -DSIFT_MDB_RELEASE='"$(SIFT_VERSION)"' \
          -Dsift_count=codec_count -Dsift_topk=codec_topk
LIBS    = -lm -lpthread -ldl

# the portable core, one list for every host and for scripts/check-portable.sh
PORTABLE := $(basename $(filter %.c,$(shell grep -v '^[\#]' $(SRC)/portable.list)))
HOSTC    := host container mb spool host_stubs
OWN      := host_regex store stubs
GLUE     := engine query feed merge
OBJS     := $(addprefix $(OUT)/,$(addsuffix .o,$(PORTABLE) $(HOSTC) $(OWN) $(GLUE)))

all: $(OUT)/sift_cli $(OUT)/libsift_udf.so $(OUT)/mdbq $(OUT)/stress

PORTOBJS := $(filter-out $(OUT)/bm25/bitmap.o,$(addprefix $(OUT)/,$(addsuffix .o,$(PORTABLE))))
$(PORTOBJS): $(OUT)/%.o: $(SRC)/%.c | $(OUT)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(HERE)/host/%.c | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

# the memory, error and string vocabulary of the SQLite host and its stubs, as they are (its host_regex.c, a POSIX ERE, is not used: host/host_regex.c is)
SQHOSTOBJS := $(addprefix $(OUT)/,host container mb spool host_stubs)
$(addsuffix .o,$(SQHOSTOBJS)): $(OUT)/%.o: $(SQHOST)/%.c | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(HERE)/%.c $(HERE)/ix.h $(HERE)/engine.h | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

# the seek limit of the bitmap cursors is a global of src/bm25/bitmap.c that a query sets while it seeks: per thread here
$(OUT)/gen/bitmap.c: $(SRC)/bm25/bitmap.c | $(OUT)/gen
	sed 's/^static uint32_t seek_limit = /static _Thread_local uint32_t seek_limit = /' $< >$@
	grep -q '_Thread_local uint32_t seek_limit' $@
$(OUT)/bm25/bitmap.o: $(OUT)/gen/bitmap.c | $(OUT)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT) $(OUT)/gen:
	mkdir -p $@

$(OUT)/sift_cli: $(OUT)/cli.o $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

# the UDFs include MariaDB headers and nothing of the engine; the engine is linked in with its symbols hidden
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

# The fallback path must run on any x86-64: outside the AVX2 and AVX-512 objects there is no POPCNT or 256/512-bit instruction, and
# nothing links libgcc's __popcountdi2 (a distribution may build it with POPCNT). Release builds run this (scripts/package-mariadb.sh).
isa-check: $(OUT)/libsift_udf.so
	@sh ../scripts/port-isa-check.sh $(OUT)/libsift_udf.so $(OUT)/*.o $(OUT)/*/*.o

.PHONY: all mdb-inc isa-check

-include $(wildcard $(OUT)/*.d $(OUT)/*/*.d)
