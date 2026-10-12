#!/usr/bin/env bash
# Fuzzers and randomized property harnesses under ASan/UBSan (test/fuzz/README.md).
#
#   scripts/fuzz.sh [--scale N] [--seed N] [TARGET...]
#
# Targets (default: all): canary codec blob kernels analyze query sql. Standalone
# harnesses are built sanitized into $SIFT_TMP/fuzz; "query" also replays its
# corpus through the server, "sql" fuzzes parse_query/tokenize in the server.
# Server-side targets run against the scripts/pgtest.sh cluster, which should
# be the ASan flavor (SANITIZE=1 scripts/pgtest.sh install; start). --scale
# multiplies the smoke-sized iteration counts of analyze, query and sql
# (default 10; codec and kernels have fixed sizes). Exit 1 on any finding.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGDATABASE=sift_fuzz PGOPTIONS='-c client_min_messages=warning' ASAN_OPTIONS=detect_leaks=0
TMP=${SIFT_TMP:-/tmp/sift-arch}
OUT=$TMP/fuzz
SCALE=10 SEED=1
SAN='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all'
while [[ ${1:-} == --* ]]; do
	case $1 in --scale) SCALE=$2 ;; --seed) SEED=$2 ;; *) sed -n '2,13p' "$0"; exit 1 ;; esac
	shift 2
done
TARGETS=("${@:-canary codec blob kernels analyze query sql}")

q() { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
asan_server() { [[ $(cat "$SIFT_ROOT/.flavor" 2>/dev/null) == asan* ]]; }
build() { nice make -s -j4 "$@" >"$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; return 1; }; }

canary() {
	asan_server || { echo "SKIP: server is not the ASan flavor"; return 0; }
	gcc -shared -fPIC -fsanitize=address -I"$(pg_config --includedir-server)" -o "$OUT/asan_canary.so" \
		"$REPO/test/fuzz/asan_canary.c" || return 1
	local before
	before=$(cat "$TMP"/sanitizer/asan.* 2>/dev/null | grep -c 'heap-buffer-overflow' || true)
	q -c "CREATE OR REPLACE FUNCTION sift_asan_canary(int) RETURNS void AS '$OUT/asan_canary.so' LANGUAGE C STRICT" \
		-c "SELECT sift_asan_canary(64)" >/dev/null 2>&1 && { echo "canary call survived"; return 1; }
	until q -c "SELECT 1" >/dev/null 2>&1; do sleep 0.5; done
	(( $(cat "$TMP"/sanitizer/asan.* | grep -c 'heap-buffer-overflow') > before )) || { echo "no ASan report"; return 1; }
	mkdir -p "$OUT/canary" && mv "$TMP"/sanitizer/asan.* "$OUT/canary/"
	echo "ASan aborted the backend and reported the overflow"
}

codec() {
	CFLAGS="$SAN" build -C "$REPO/tools/codec" OUT="$OUT/codec" "$OUT/codec/test" && "$OUT/codec/test"
}

blob() {
	CFLAGS="$SAN" build -C "$REPO/tools/codec" OUT="$OUT/codec" "$OUT/codec/fuzz" && "$OUT/codec/fuzz" -s "$SEED" -n $((2000 * SCALE))
}

kernels() {
	build -C "$REPO/tools/kernels" SANITIZE=1 OUT="$OUT/kernels" "$OUT/kernels/test" && "$OUT/kernels/test"
}

analyze() {
	build -C "$REPO/tools/analyze" -B SANITIZE=1 harness && mv "$REPO/tools/analyze/harness" "$OUT/analyze-harness" &&
		"$OUT/analyze-harness" fuzz $((20000 * SCALE)) "$SEED"
}

query() {
	build -C "$REPO/tools/query" -B harness && mv "$REPO/tools/query/harness" "$OUT/query-harness" &&
		python3 -I "$REPO/tools/query/gen.py" --mode fuzz -n $((2000 * SCALE)) --seed "$SEED" -o "$OUT/queries.copy" &&
		"$OUT/query-harness" "$OUT/queries.copy" && q -v file="$OUT/queries.copy" -f "$REPO/tools/query/fuzz.sql"
}

sql() {
	local log=$TMP/postgres.log off
	off=$(stat -c %s "$log")
	q -v sift_ref_lib=1 -f "$REPO/test/sql/reference.sql" -f "$REPO/test/diff/diff.sql" -f "$REPO/test/fuzz/fuzz.sql" \
		-c "TRUNCATE sift_fuzz.findings" -c "SELECT sift_diff.build(0.$SEED, 50, 300)" >/dev/null || return 1
	if ! q -c "SELECT * FROM sift_fuzz.run(0.$SEED, $((100 * SCALE)))"; then
		echo "server error; last input: $(tail -c +"$off" "$log" | grep -a 'sift_fuzz input' | tail -1)"
		return 1
	fi
	q -P format=aligned -c "SELECT kind, left(input, 100) AS input, detail FROM sift_fuzz.findings ORDER BY kind LIMIT 20"
	# parser disagreements are grammar questions for the spec, reported but not fatal
	(( $(q -c "SELECT count(*) FROM sift_fuzz.findings WHERE kind <> 'disagree'") == 0 ))
}

mkdir -p "$OUT"
psql -X -qAt -d postgres -c "DROP DATABASE IF EXISTS sift_fuzz WITH (FORCE)" -c "CREATE DATABASE sift_fuzz"
q -c "CREATE EXTENSION sift"
status=0
for t in ${TARGETS[*]}; do
	echo "== fuzz $t"
	if "$t" 2>&1 | tail -25; then
		echo "PASS fuzz $t"
	else
		echo "FAIL fuzz $t"
		status=1
	fi
done
if asan_server; then
	"$REPO/scripts/pgtest.sh" sanitizer-report || { echo "FAIL server sanitizer reports"; status=1; }
fi
exit $status
