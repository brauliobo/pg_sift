# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its path from src/ (#include "bm25/query/query.h"): SIFT_INC has -I$(SRC). SIFT_DIRS is the list of
# the folders. They are not on the vpath: a file name is not unique across them (am.c, kern.c, ...), so a rule names the path of its source.
SIFT_DIRS    := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INCDIRS := $(SRC)
SIFT_INC     := $(addprefix -I,$(SIFT_INCDIRS))
# The code generation baseline of every build of src/: portable x86-64 (the vector kernels opt in per function with target attributes) and plain armv8-a on
# Linux/AArch64 (NEON, no LSE or crypto: a distribution's pg_config may carry -march=armv8.2-a+crypto, which would not run on every Arm CPU); the compiler's own
# default elsewhere, where -march=x86-64 is an error. The target is the one of the compiler that builds (PGXS names it: the first use is after it is included), not the host's.
SIFT_TARGET = $(eval SIFT_TARGET := $(shell $(CC) -dumpmachine))$(SIFT_TARGET)
SIFT_LINUX_ARM64 = $(and $(findstring aarch64,$(SIFT_TARGET)),$(findstring linux,$(SIFT_TARGET)))
SIFT_BASE_ISA = $(if $(findstring x86_64,$(SIFT_TARGET)),-march=x86-64 -mtune=generic,$(if $(SIFT_LINUX_ARM64),-march=armv8-a -mtune=generic))
# The include directories of the server headers' own dependencies (libintl.h from Homebrew's gettext), for the builds outside PGXS that include postgres.h.
SIFT_PGCPP := $(shell pg_config --cppflags)
# The matching link flags: the library directories (lz4, zstd) and libintl, which the server's libpgport needs where gettext is separate (not the whole of --libs: -lnuma and
# -lxml2 are not needed, and --ldflags may carry -flto).
SIFT_PGLIBS := $(filter -L% -lintl,$(shell pg_config --ldflags --libs))
# SIFT_FMV=auto (default) | off. auto compiles the hot leaf loops of core/fmv.h as IFUNC clones on glibc Linux (x86-64 GCC 11 or Clang 16; AArch64 GCC 14 or a Clang that emits
# the resolver) and as target() functions picked from the CPU flags elsewhere; off compiles the baseline body only (-DSIFT_FMV_DISABLE).
SIFT_FMV ?= auto
# Clang on Linux/AArch64 drops target_clones without a libgcc of GCC 14 (or compiler-rt): the probe says whether it emitted the resolver (run once, on the first use)
SIFT_FMV_ARM64_PROBE = printf '__attribute__((target_clones("default","sve"))) int f(int x){return x;}\n' | $(CC) -O2 -fPIC -march=armv8-a -S -x c - -o - 2>/dev/null | grep -q 'f\.resolver' && echo 1
SIFT_FMV_CLANG_ARM64 = $(eval SIFT_FMV_CLANG_ARM64 := $(if $(SIFT_LINUX_ARM64),$(shell $(SIFT_FMV_ARM64_PROBE))))$(SIFT_FMV_CLANG_ARM64)
SIFT_FMV_CPPFLAGS = $(if $(filter off,$(SIFT_FMV)),-DSIFT_FMV_DISABLE=1,$(if $(filter 1,$(SIFT_FMV_CLANG_ARM64)),-DSIFT_CLANG_AARCH64_FMV=1))
