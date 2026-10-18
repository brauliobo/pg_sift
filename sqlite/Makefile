# SQLite loadable extension around the portable pg_sift sources.
#   nice make -j4                builds build/sift.so (load with .load build/sift); SIFT_VERSION=x.y.z overrides the version in VERSION
#   make test                    ./test.sh
#   make isa-check               no POPCNT or vector instruction outside the AVX2/AVX-512 objects (the baseline runs on any x86-64)
#   make SIMD=none B=build-scalar   no vector code at all (the vector levels dispatch to the scalar tables): the compile-time scalar build
#   make asan | tsan | tsan-run  sanitizer builds (build-asan/, build-tsan/); tsan-run also runs the C stress test on the TSan build
#   make span-test               the unit and adversarial tests of src/sift_span.h (docs/SAFETY.md); make span-test-asan under ASan+UBSan;
#                                make SIMD=none B=build-scalar span-test with the vector-free flags
#   make cross-aarch64           compiles everything for aarch64 with clang (no execution)
# The sources of ../src listed in portable.list are compiled unmodified, against the host implementation in host/ (they include
# only src/sift_host.h, which selects it with -DSIFT_HOST_SQLITE), except two mechanical changes: sift_bitmap.c gets its scan-limit global made thread-local (copied and patched with sed into
# $(B)/gen, src/ is untouched) and sift_prefix.c's one entry point is renamed so that sqinit.c can serialize it.
SRC := ../src
B ?= build
# The release version: the repository tag without the "v" (scripts/package-sqlite.sh checks that VERSION matches the tag)
SIFT_VERSION ?= $(shell cat VERSION)
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
SIMD_SRC := sift_bitmap_avx2 sift_bitmap_avx512 sift_kernels_avx2 sift_kernels_avx512
ifeq ($(SIMD),none)
EXTRA += -DSQ_SIMD_NONE
ALIASES := -Dsift_kern_avx2=sift_kern_scalar -Dsift_kern_avx512=sift_kern_scalar -Dsift_kernels_avx2=sift_kernels_scalar \
	-Dsift_kernels_avx2_nopdep=sift_kernels_scalar -Dsift_kernels_avx512=sift_kernels_scalar $(CPUPROBE)
endif
CFLAGS += $(OPT) -g -ffp-contract=off -fwrapv -fno-strict-aliasing -fPIC -Wall -Wno-misleading-indentation \
	-Wno-unused-function -Wno-declaration-after-statement -MMD -D_GNU_SOURCE '-DSQ_VERSION="$(SIFT_VERSION)"' $(ISA) $(EXTRA) -DSIFT_HOST_SQLITE -Ihost -I$(SRC) -I.
LDLIBS := -lm -lpthread

# the portable core, one list for every host and for the check (scripts/check-portable.sh); the vector files only when there are vector kernels
PORTABLE := $(filter-out $(if $(filter none,$(SIMD)),$(SIMD_SRC)),$(basename $(filter %.c,$(shell grep -v '^[#]' $(SRC)/portable.list))))
OWN := host host_regex sqstore sqstubs sqbuild sqmerge sqvtab sqaux sqpar sqinit
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

# sift_version() reports it: rebuild sqinit.o when it changes
$(B)/gen/version: FORCE | $(B)/gen
	@echo '$(SIFT_VERSION)' | cmp -s - $@ || echo '$(SIFT_VERSION)' > $@
$(B)/sqinit.o: $(B)/gen/version
FORCE:

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: host/%.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B) $(B)/gen:
	mkdir -p $@

check test: all
	./test.sh

# The fallback path must run on any x86-64: outside the AVX2 and AVX-512 objects there is no POPCNT or 256/512-bit instruction, and
# nothing links libgcc's __popcountdi2 (a distribution may build it with POPCNT). Release builds run this (scripts/package-sqlite.sh).
isa-check: all
	@bad=0; n=0; for o in $(B)/*.o; do case $$o in *_avx2.o|*_avx512.o) continue;; esac; n=$$((n + 1)); \
		if objdump -d $$o | grep -qE '\<(popcnt|[yz]mm[0-9]+)\>'; then echo "baseline object uses POPCNT or vector registers: $$o"; bad=1; fi; done; \
	if nm $(B)/sift.so | grep -qi '__popcount.i2'; then echo "sift.so links a libgcc popcount helper"; bad=1; fi; \
	[ $$bad = 0 ] && echo "isa-check: ok, $$n baseline objects without POPCNT or vector instructions, no libgcc popcount helper"; exit $$bad

# The C stress test: N threads, each with a connection, querying while one writes. The extension is linked in and
# registered with sqlite3_auto_extension.
$(B)/stress: tests/stress.c $(OBJS) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/stress.c $(OBJS) $(LDLIBS) -lsqlite3

# the checked reader of the portable core: C library only, no host, no database
$(B)/span_test: tests/span_test.c $(SRC)/sift_span.h $(SRC)/sift_varint.h | $(B)
	$(CC) $(CFLAGS) -Wextra -Wno-sign-compare -Wno-unused-parameter -o $@ tests/span_test.c

span-test: $(B)/span_test
	$(B)/span_test

span-test-asan:
	$(MAKE) B=build-asan EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" OPT=-O1 build-asan/span_test
	build-asan/span_test

asan:
	$(MAKE) B=build-asan EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -fno-sanitize=nonnull-attribute" OPT=-O1 build-asan/sift.so build-asan/stress

tsan:
	$(MAKE) B=build-tsan EXTRA="-fsanitize=thread -fno-omit-frame-pointer" OPT=-O1 build-tsan/stress

# the stress test on the TSan build: separate connections, one shared connection, with and without parallel scans
tsan-run: tsan
	for a in "-m multi -r 4" "-m serialized -r 4" "-c -r 4" "-m multi -r 3 -p 3" "-c -r 3 -p 3"; do \
		TSAN_OPTIONS="suppressions=$(CURDIR)/tests/tsan.supp exitcode=66" build-tsan/stress -s 10 $$a || exit 1; done

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

.PHONY: FORCE all check test isa-check clean asan tsan tsan-run cross-aarch64 cross-objs span-test span-test-asan
