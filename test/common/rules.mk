# The Make variables and rules every standalone C harness shares (test/unit, test/fuzz, test/scaling and tools/*): the compiler, the PostgreSQL headers
# and libraries of PG_CONFIG, the code generation flags of src/areas.mk, the sanitizer modes and the rule that builds a stand-in of this directory.
#   PG_CONFIG=/path/to/pg_config   builds against that PostgreSQL (its bin directory comes first on PATH, where src/areas.mk looks)
# The includer may set SRC (default: the src/ of this tree) before including it.
COMMON         := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
ROOT           := $(COMMON)/../..
SRC            ?= $(ROOT)/src
PG_CONFIG      ?= pg_config
export PATH    := $(if $(findstring /,$(PG_CONFIG)),$(dir $(PG_CONFIG)):)$(PATH)
include $(SRC)/areas.mk

CC             ?= gcc
PGINC          := $(shell $(PG_CONFIG) --includedir-server)
PGLIB          := -L$(shell $(PG_CONFIG) --libdir)
PGLIBS         := $(PGLIB) -lpgport -lpgcommon $(SIFT_PGLIBS)
FLOCK           = $(if $(shell command -v flock),flock /tmp/sift-perf.lock)
SIFT_TEST_INC  := -I$(ROOT)

SAN_ASAN       := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
OPT_plain       = -O2
OPT_asan        = -O1 $(SAN_ASAN)
OPT_tsan        = -O1 -fsanitize=thread
ENV_plain       =
ENV_asan        = ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1
ENV_tsan        = TSAN_OPTIONS=halt_on_error=1:exitcode=66

# $(call common_obj,DIR,NAME): the rule that builds DIR/NAME.o from test/common/NAME.c with the includer's CFLAGS and CFLAGS_NAME
define common_obj
$(1)/$(2).o: $(COMMON)/$(2).c $(wildcard $(COMMON)/*.h) $(COMMON)/trapped.def
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(CFLAGS_$(2)) -c -o $$@ $$<
endef
