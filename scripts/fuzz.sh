#!/usr/bin/env bash
# Fuzzers and randomized property harnesses under ASan/UBSan (test/fuzz/README.md).
#
#   scripts/fuzz.sh [--scale N] [--seed N] [--budget SEC] [TARGET...]
#
# Targets (default: all): canary codec blob kernels analyze query sql colchunk bm25 shred (bm25-raw is opt-in). Standalone
# harnesses are built sanitized into $SIFT_TMP/fuzz; "query" also replays its
# corpus through the server, "sql" fuzzes parse_query/tokenize in the server.
# Server-side targets run against the scripts/pgtest.sh cluster, which should
# be the ASan flavor (SANITIZE=1 scripts/pgtest.sh install; start). --scale
# multiplies the iteration counts of every target (default 10: about 3.5
# minutes without the server sanitizer). --budget runs scale-1 passes
# (about 25 s) until SEC seconds have passed, seeded targets with a new seed
# each pass. The standalone harnesses run with LeakSanitizer (their palloc stubs free at exit, tools/pgarena.h),
# the server with the leak gate of scripts/leakcheck.sh. colchunk, bm25 and shred are the coverage-guided targets of test/fuzz (libFuzzer
# under clang, else the portable driver): the decoders of the column chunks, of the term blobs and of the shredder, from seeds the
# writers make, for 6 s per --scale step (60 s at the default), with the findings recorded in the design notes skipped by
# FZ_SKIP / test/fuzz/ubsan.supp until they are fixed (FZ_SKIP= and FZ_NO_SUPP=1 run strict); bm25-raw is the query path on bytes
# nobody verified, which fails today. Exit 1 on any finding.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGDATABASE=sift_fuzz PGOPTIONS='-c client_min_messages=warning' ASAN_OPTIONS=detect_leaks=1
TMP=${SIFT_TMP:-/tmp/sift-arch}
OUT=$TMP/fuzz
SCALE=10 SEED=1 BUDGET=0
SAN='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all'
while [[ ${1:-} == --* ]]; do
	case $1 in --scale) SCALE=$2 ;; --seed) SEED=$2 ;; --budget) BUDGET=$2 ;; *) sed -n '2,16p' "$0"; exit 1 ;; esac
	shift 2
done
TARGETS=("${@:-canary codec blob kernels analyze query sql colchunk bm25 shred}")
(( BUDGET == 0 )) || SCALE=1

q() { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
asan_server() { [[ $(cat "$SIFT_ROOT/.flavor" 2>/dev/null) == asan* ]]; }
build() { nice make -s -j4 "$@" >"$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; return 1; }; }

canary() {
	asan_server || { echo "SKIP: server is not the ASan flavor"; return 0; }
	gcc -shared -fPIC -fsanitize=address -I"$(pg_config --includedir-server)" -o "$OUT/asan_canary.so" \
		"$REPO/test/fuzz/asan_canary.c" || return 1
	local before
	before=$(cat "$TMP"/sanitizer/asan.* 2>/dev/null | grep -c 'heap-buffer-overflow' || true)
	"$REPO/scripts/leakcheck.sh" expect 1 'terminated by signal 6'	# the aborted backend is logged by the postmaster
	q -c "CREATE OR REPLACE FUNCTION sift_asan_canary(int) RETURNS void AS '$OUT/asan_canary.so' LANGUAGE C STRICT" \
		-c "SELECT sift_asan_canary(64)" >/dev/null 2>&1 && { echo "canary call survived"; return 1; }
	until q -c "SELECT 1" >/dev/null 2>&1; do sleep 0.5; done
	(( $(cat "$TMP"/sanitizer/asan.* | grep -c 'heap-buffer-overflow') > before )) || { echo "no ASan report"; return 1; }
	mkdir -p "$OUT/canary" && mv "$TMP"/sanitizer/asan.* "$OUT/canary/"
	echo "ASan aborted the backend and reported the overflow"
}

codec() {
	CFLAGS="$SAN" build -C "$REPO/tools/codec" OUT="$OUT/codec" "$OUT/codec/test" && "$OUT/codec/test" $((300 * SCALE))
}

blob() {
	CFLAGS="$SAN" build -C "$REPO/tools/codec" OUT="$OUT/codec" "$OUT/codec/fuzz" && "$OUT/codec/fuzz" -s "$SEED" -n $((2000 * SCALE))
}

kernels() {
	build -C "$REPO/tools/kernels" SANITIZE=1 OUT="$OUT/kernels" "$OUT/kernels/test" &&
		"$OUT/kernels/test" "$(awk -v s="$SCALE" 'BEGIN { print s / 5 }')"
}

analyze() {
	build -C "$REPO/tools/analyze" -B SANITIZE=1 harness && mv "$REPO/tools/analyze/harness" "$OUT/analyze-harness" &&
		"$OUT/analyze-harness" fuzz $((20000 * SCALE)) "$SEED"
}

# libfuzz TARGET: a test/fuzz target for 6 s per scale step; a kept corpus in $OUT/lf-TARGET, crash files in $OUT/lf-TARGET/artifacts
libfuzz() {
	local t=$1 d=$OUT/lf-$1 known=${FZ_SKIP-} supp="$REPO/test/fuzz/ubsan.supp"
	[[ -v FZ_SKIP ]] || case $t in colchunk) known=lut ;; esac		# findings of the design notes that are not fixed yet
	[[ -z ${FZ_NO_SUPP:-} ]] || supp=/dev/null
	build -C "$REPO/test/fuzz" OUT="$OUT/lf" "$OUT/lf/$t" || return 1
	rm -rf "$d/seeds" && mkdir -p "$d/corpus" "$d/artifacts"
	FZ_GEN="$d/seeds" "$OUT/lf/$t" >/dev/null 2>&1 && cp -n "$d"/seeds/* "$d/corpus/" || { echo "no seeds"; return 1; }
	env FZ_SKIP="$known" ${FZ_MODE:+FZ_MODE=$FZ_MODE} UBSAN_OPTIONS="suppressions=$supp:print_stacktrace=1" \
		"$OUT/lf/$t" -max_total_time=$((6 * SCALE)) -seed="$SEED" -timeout=10 -rss_limit_mb=4096 -artifact_prefix="$d/artifacts/" "$d/corpus" 2>&1 |
		grep -v -E '^(#[0-9]+\s+(NEW|REDUCE|pulse)|MS:|stat::|\s*$|"|#+ )'
}

colchunk() { libfuzz colchunk; }
shred() { libfuzz shred; }
# the checked blobs only: sift_blob_check must keep every cursor inside the blob (the unverified query path is bm25-raw)
bm25() { FZ_MODE=1 libfuzz bm25; }
bm25-raw() { FZ_MODE= libfuzz bm25; }

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
q -c "CREATE EXTENSION pg_sift"
status=0
run() {
	local t
	for t in "$@"; do
		echo "== fuzz $t (seed $SEED)"
		if "$t" 2>&1 | tail -25; then
			echo "PASS fuzz $t"
		else
			echo "FAIL fuzz $t"
			status=1
		fi
	done
}
deadline=$((SECONDS + BUDGET))
run ${TARGETS[*]}
seeded=$(printf '%s\n' ${TARGETS[*]} | grep -x -E 'blob|analyze|query|sql|colchunk|bm25|shred' || true)
while [[ -n $seeded ]] && (( SECONDS < deadline && status == 0 )); do
	SEED=$((SEED + 1))
	run $seeded
done
if asan_server; then
	"$REPO/scripts/pgtest.sh" sanitizer-report || { echo "FAIL server sanitizer reports"; status=1; }
fi
exit $status
