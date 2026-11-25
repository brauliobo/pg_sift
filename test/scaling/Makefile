# The deterministic scaling gate of the C data structures (check_scaling_c.c). The sources under test are compiled with the coverage instrumentation that
# op_counters.h reads; SAN=1 adds AddressSanitizer and UBSan. `make -C test/scaling run`, `run SAN=1`, `negative`.
include ../common/rules.mk
OUT     ?= /tmp/sift-scaling-c$(if $(SAN),-san)
SHELL    = bash
COV      = -fsanitize-coverage=trace-pc,trace-cmp
SANFLAGS = $(if $(SAN),$(SAN_ASAN))
CFLAGS   = -std=gnu11 -fno-pie -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare -I. $(SIFT_INC) -I$(PGINC) -D_GNU_SOURCE $(SANFLAGS)
LDFLAGS  = $(SANFLAGS) -no-pie -Wl,--wrap=memcpy,--wrap=memmove -Wl,--unresolved-symbols=ignore-all
LDLIBS   = -lm

include cases.mk

HARNESS  = $(OUT)/check_scaling_c.o $(OUT)/op_counters.o $(OUT)/pg_shim_ops.o
INSTR    = $(patsubst %.c,$(OUT)/%.o,$(CASES)) $(patsubst $(SRC)/%.c,$(OUT)/src/%.o,$(TARGETS)) $(patsubst $(SRC)/../sqlite/host/%.c,$(OUT)/host/%.o,$(HOSTSRC))

all: $(OUT)/check_scaling_c

# the link leaves the symbols of the server unresolved (a case never reaches them), so a coverage hook that op_counters.c lacks would be called at address 0: it is checked here
$(OUT)/check_scaling_c: $(HARNESS) $(INSTR)
	@missing=$$(comm -23 <(nm -u $(INSTR) | awk '/__sanitizer_cov/ {print $$2}' | sort -u) <(nm --defined-only $(OUT)/op_counters.o | awk '{print $$3}' | sort -u)); \
	  [ -z "$$missing" ] || { echo "op_counters.c does not define: $$missing"; exit 1; }
	@$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(HARNESS): $(OUT)/%.o: %.c op_counters.h cases.h
	@mkdir -p $(@D)
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

$(patsubst %.c,$(OUT)/%.o,$(CASES)): $(OUT)/%.o: %.c op_counters.h
	@mkdir -p $(@D)
	@echo CC $< '(instrumented)'
	@$(CC) $(CFLAGS) $(COV) -c -o $@ $<

$(OUT)/src/%.o: $(SRC)/%.c
	@mkdir -p $(@D)
	@echo CC $< '(instrumented)'
	@$(CC) $(CFLAGS) $(COV) -c -o $@ $<

$(OUT)/host/%.o: $(SRC)/../sqlite/host/%.c
	@mkdir -p $(@D)
	@echo CC $< '(instrumented)'
	@$(CC) $(CFLAGS) $(COV) -c -o $@ $<

run: $(OUT)/check_scaling_c
	$(OUT)/check_scaling_c

negative: $(OUT)/check_scaling_c
	$(OUT)/check_scaling_c --negative-test

clean:
	rm -rf /tmp/sift-scaling-c /tmp/sift-scaling-c-san

.PHONY: all run negative clean
