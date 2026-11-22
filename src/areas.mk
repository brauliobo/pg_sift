# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its path from src/ (#include "bm25/query.h"): SIFT_INC has -I$(SRC). SIFT_DIRS is the list of
# the folders. They are not on the vpath: a file name is not unique across them (am.c, kern.c, ...), so a rule names the path of its source.
SIFT_DIRS    := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INCDIRS := $(SRC)
SIFT_INC     := $(addprefix -I,$(SIFT_INCDIRS))
# The code generation baseline of every build of src/: portable x86-64 (the vector kernels opt in per function with target attributes) and plain armv8-a on
# Linux/AArch64 (NEON, no LSE or crypto: a distribution's pg_config may carry -march=armv8.2-a+crypto, which would not run on every Arm CPU); the compiler's own
# default elsewhere, where -march=x86-64 is an error.
SIFT_BASE_ISA := $(if $(filter x86_64,$(shell uname -m)),-march=x86-64 -mtune=generic,$(if $(filter Linux-aarch64,$(shell uname -s)-$(shell uname -m)),-march=armv8-a -mtune=generic))
# The include directories of the server headers' own dependencies (libintl.h from Homebrew's gettext), for the builds outside PGXS that include postgres.h.
SIFT_PGCPP := $(shell pg_config --cppflags)
# The matching link flags: the library directories (lz4, zstd) and libintl, which the server's libpgport needs where gettext is separate (not the whole of --libs: -lnuma and
# -lxml2 are not needed, and --ldflags may carry -flto).
SIFT_PGLIBS := $(filter -L% -lintl,$(shell pg_config --ldflags --libs))
# SIFT_FMV=auto (default) | off. auto compiles the hot leaf loops of core/fmv.h as IFUNC clones on glibc Linux (x86-64 GCC 11 or Clang 16; AArch64 GCC 14 or a Clang that emits
# the resolver) and as target() functions picked from the CPU flags elsewhere; off compiles the baseline body only (-DSIFT_FMV_DISABLE).
SIFT_FMV ?= auto
ifeq ($(SIFT_FMV),off)
SIFT_FMV_CPPFLAGS := -DSIFT_FMV_DISABLE=1
else
SIFT_FMV_CPPFLAGS :=
# Clang on Linux/AArch64 drops target_clones without a libgcc of GCC 14 (or compiler-rt): the flag says it emitted the resolver
ifeq ($(shell uname -s)-$(shell uname -m),Linux-aarch64)
SIFT_FMV_CLANG_ARM64 := $(shell printf '__attribute__((target_clones("default","sve"))) int f(int x){return x;}\n' | $(CC) -O2 -fPIC -march=armv8-a -S -x c - -o - 2>/dev/null | grep -q 'f\.resolver' && echo 1)
ifeq ($(SIFT_FMV_CLANG_ARM64),1)
SIFT_FMV_CPPFLAGS += -DSIFT_CLANG_AARCH64_FMV=1
endif
endif
endif
