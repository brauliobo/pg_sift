#!/bin/bash
# unit-scaling.sh [OUT]: builds each test/unit/*_test.c with ASan and UBSan and runs it; the calls it makes into the server are stubs that abort, so a test that reaches one fails
set -euo pipefail
export LC_ALL=C
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-/dev/stdout}
W=$(mktemp -d "${TMPDIR:-/tmp}/sift-unit-XXXXXX")
trap 'rm -rf "$W"' EXIT
INC=$(pg_config --includedir-server)
FLAGS=(-std=gnu17 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
	-D_GNU_SOURCE -DSIFT_TESTING -I"$REPO" -I"$REPO/src" -I"$INC" -isystem /usr/include/libxml2)
: >"$OUT"
rc=0
for src in "$REPO"/test/unit/*_test.c; do
	name=$(basename "$src" .c)
	cc "${FLAGS[@]}" -c "$src" -o "$W/$name.o"
	: >"$W/$name.stubs.c"
	for try in 1 2 3; do
		cc "${FLAGS[@]}" "$W/$name.o" "$W/$name.stubs.c" -o "$W/$name" 2>"$W/link.log" && break
		{ grep -o "undefined reference to \`[A-Za-z0-9_]*'" "$W/link.log" | sed "s/.*\`//; s/'//" | sort -u | while read -r s; do echo "void $s(void) { __builtin_trap(); }"; done >>"$W/$name.stubs.c"; } || true
	done
	cc "${FLAGS[@]}" "$W/$name.o" "$W/$name.stubs.c" -o "$W/$name"
	echo "== $name" >>"$OUT"
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 nice "$W/$name" >>"$OUT" 2>&1 || rc=1
done
exit $rc
