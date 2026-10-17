# SQLite loadable extension around the portable pg_sift sources.
#   nice make -j4                builds build/sift.so (load with .load build/sift)
#   make test                    ./test.sh
#   make SIMD=none B=build-scalar   no vector code at all (the vector levels dispatch to the scalar tables): the compile-time scalar build
#   make asan | tsan             sanitizer builds (build-asan/, build-tsan/) and the C stress test on them
#   make cross-aarch64           compiles everything for aarch64 with clang (no execution)
# The sources of ../src listed in PORTABLE are compiled unmodified, against the PostgreSQL API shim in shim/, except two
# mechanical changes: sift_bitmap.c gets its scan-limit global made thread-local (copied and patched with sed into
# $(B)/gen, src/ is untouched) and sift_prefix.c's one entry point is renamed so that sqinit.c can serialize it.
SRC := ../src
B ?= build
CC ?= cc
OPT ?= -O2
ARCH := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
SIMD ?= auto
# SIFT_ISA: the x86-64 baseline, SIMD kernels opt in per function (see src/sift_cpu.h); -ffp-contract=off keeps scores bit-identical.
# Other architectures have no vector kernels (SIMD=none): the vector levels dispatch to the scalar tables, and the x86 CPU probes
# of sift_bitmap.c answer "no"
ifeq ($(ARCH),x86_64)
ISA := -march=x86-64 -mtune=generic
else
SIMD := none
CPUPROBE := '-D__builtin_cpu_init()=' '-D__builtin_cpu_supports(x)=0'
endif
ifneq ($(SIMD),none)
SIMD_SRC := sift_bitmap_avx2 sift_bitmap_avx512 sift_kernels_avx2 sift_kernels_avx512
else
EXTRA += -DSQ_SIMD_NONE
ALIASES := -Dsift_kern_avx2=sift_kern_scalar -Dsift_kern_avx512=sift_kern_scalar -Dsift_kernels_avx2=sift_kernels_scalar \
	-Dsift_kernels_avx2_nopdep=sift_kernels_scalar -Dsift_kernels_avx512=sift_kernels_scalar $(CPUPROBE)
endif
CFLAGS += $(OPT) -g -ffp-contract=off -fwrapv -fno-strict-aliasing -fPIC -Wall -Wno-misleading-indentation \
	-Wno-unused-function -Wno-declaration-after-statement -MMD -D_GNU_SOURCE $(ISA) $(EXTRA) -Ishim -I$(SRC) -I.
LDLIBS := -lm -lpthread

PORTABLE := sift_bitmap sift_bitmap_scalar sift_kernels sift_analyze sift_analyze_stem sift_query sift_posting sift_dict \
	sift_segment sift_prefix sift_filter sift_eval sift_cpu $(SIMD_SRC)
OWN := shim shim_regex sqstore sqstubs sqbuild sqmerge sqvtab sqaux sqpar sqinit
OBJS := $(addprefix $(B)/,$(addsuffix .o,$(PORTABLE) $(OWN)))

all: $(B)/sift.so

$(B)/sift.so: $(OBJS)
	$(CC) -shared $(EXTRA) -o $@ $(OBJS) $(LDLIBS)

# sift_bitmap.c: the scan limit of sift_seek_eq is a global written during scans; one per thread
$(B)/gen/sift_bitmap.c: $(SRC)/sift_bitmap.c | $(B)/gen
	sed 's/^static uint32_t seek_limit = /static _Thread_local uint32_t seek_limit = /' $< > $@
	grep -q '_Thread_local uint32_t seek_limit' $@

$(B)/sift_bitmap.o: $(B)/gen/sift_bitmap.c | $(B)
	$(CC) $(CFLAGS) $(ALIASES) -c -o $@ $<

# sift_prefix.c keeps a process-wide cache of wide-prefix unions; sqinit.c wraps the entry point in a mutex
$(B)/sift_prefix.o: $(SRC)/sift_prefix.c | $(B)
	$(CC) $(CFLAGS) -Dsift_prefix_union=sq_real_sift_prefix_union -c -o $@ $<

# SIMD=none: the dispatch tables of the vector levels are the scalar ones (the vector files are not built)
$(B)/sift_kernels.o: $(SRC)/sift_kernels.c | $(B)
	$(CC) $(CFLAGS) $(ALIASES) -c -o $@ $<

$(B)/%.o: $(SRC)/%.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B) $(B)/gen:
	mkdir -p $@

check test: all
	./test.sh

# The C stress test: N threads, each with a connection, querying while one writes. The extension is linked in and
# registered with sqlite3_auto_extension.
$(B)/stress: tests/stress.c $(OBJS) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/stress.c $(OBJS) $(LDLIBS) -lsqlite3

asan:
	$(MAKE) B=build-asan EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -fno-sanitize=nonnull-attribute" OPT=-O1 build-asan/sift.so build-asan/stress

tsan:
	$(MAKE) B=build-tsan EXTRA="-fsanitize=thread -fno-omit-frame-pointer" OPT=-O1 build-tsan/stress

# Without an aarch64 sysroot the build machine's x86-64 glibc headers stand in: bits/floatn.h is skipped and bits/wordsize.h and bits/pthreadtypes-arch.h are
# replaced by LP64 ones (generated), so only the types and the code are checked, nothing is linked or run.
cross-aarch64:
	mkdir -p build-aarch64/gen/inc/bits
	printf '#define __WORDSIZE 64\n#define __WORDSIZE_TIME64_COMPAT32 1\n#define __SYSCALL_WORDSIZE 64\n' > build-aarch64/gen/inc/bits/wordsize.h
	sed 's/__attribute__ ((__regparm__ (1)))//' /usr/include/bits/pthreadtypes-arch.h > build-aarch64/gen/inc/bits/pthreadtypes-arch.h
	$(MAKE) B=build-aarch64 CC="clang --target=aarch64-linux-gnu" EXTRA="-fsyntax-only -Wno-unused-command-line-argument -D_BITS_FLOATN_H -isystem build-aarch64/gen/inc" cross-objs

cross-objs: $(OBJS)

clean:
	rm -rf build build-asan build-tsan build-scalar build-aarch64

-include $(wildcard $(B)/*.d)

.PHONY: all check test clean asan tsan cross-aarch64 cross-objs
