#!/bin/bash
# test/bench_on/run.sh: the standalone harnesses of the header-only structures, built with ASan and UBSan and run: a1 (core/u64set.h), tbl_a3 (table/f2/f2_lock_touched.c) and v1_a4 (table/v1/keyidx.h)
set -euo pipefail
cd "$(dirname "$0")/../.."
inc=$(pg_config --includedir-server)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
for b in bench_on_core_a1 bench_on_tbl_a3 bench_on_v1_a4; do
	gcc -std=gnu17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -D_GNU_SOURCE \
		-I. -Isrc -I"$inc" -o "$out/$b" "test/bench_on/$b.c"
	ASAN_OPTIONS=detect_leaks=0 "$out/$b"
done
