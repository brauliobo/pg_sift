# Column codec research harness (no backend linked).
#   nice make -C tools/colcodec research ARGS='-s'   encoding/compressor sizes on generated e-commerce orders (-s adds decode speeds, -k clustered order)
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -ffp-contract=off -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare
LDLIBS   = -lm -lzstd -llz4
OUT     ?= /tmp/sift-col-compress/out

$(OUT)/research: research.c gen.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ research.c $(LDLIBS)

research: $(OUT)/research
	flock /tmp/sift-perf.lock $(OUT)/research $(ARGS)

clean:
	rm -rf $(OUT)

.PHONY: research clean
