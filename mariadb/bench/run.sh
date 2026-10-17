#!/usr/bin/env bash
# Latency of sift vs InnoDB FULLTEXT on the loaded Wikipedia table, one client, warm cache (every file runs twice, the second
# pass is reported), plus the per-call cost of a UDF returning JSON.
#   mariadb/bench/run.sh TRACE        env: SIFT_MDB_ROOT, MDBQ (default $SIFT_BUILD/mdbq), CLIENT_CPU (taskset list of the client),
#                                     REPS (passes per file, default 2), QUERIES (per class, default 1000), CELLS (regex on the row labels: run only those)
# TRACE: the benchmark trace (id TAB and|or|phrase TAB terms), 1000 queries per class. Database "bench", table docs (id, body)
# with a FULLTEXT index on body, sift index "wiki".
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${SIFT_MDB_ROOT:-/tmp/sift-mariadb}
MDBQ=${MDBQ:-${SIFT_BUILD:-/tmp/sift-mariadb-build}/mdbq}
PIN=${CLIENT_CPU:+taskset -c $CLIENT_CPU}
W=$(mktemp -d "$ROOT/tmp/bench.XXXXXX")
trap 'rm -rf "$W"' EXIT

# run LABEL KIND CLASS: prints "LABEL median_us qps"
run() {
  [[ -z ${CELLS:-} || $4 =~ $CELLS ]] || return 0
  python3 -I "$HERE/mkstmts.py" "$2" "$1" "$3" --limit="${QUERIES:-1000}" >"$W/s.sql"
  $PIN "$MDBQ" "$ROOT/mysql.sock" bench time "$W/s.sql" "${REPS:-2}" | awk -v l="$4" '{ print l, $6, $(NF) }'
}
cell() { run "$TRACE" "$1" "$2" "$3"; }

TRACE=$1
echo "# load average: $(cut -d' ' -f1-3 /proc/loadavg), client cpu: ${CLIENT_CPU:-unpinned}"
for c in and or phrase; do
  cell sift-topk-dt "$c" "$c sift-join"
  cell sift-raw "$c" "$c sift-udf"
  if [[ $c == or ]]; then
    cell inno-topk "$c" "$c inno-natural"
    cell inno-topk-bool "$c" "$c inno-boolean"
  else
    cell inno-topk "$c" "$c inno-boolean"
  fi
  cell sift-count "$c" "$c count-sift"
  cell inno-count "$c" "$c count-inno"
done

# per-call cost: the protocol floor, a UDF returning JSON, JSON_TABLE over it, and the join an application adds
COLS="(n FOR ORDINALITY, id BIGINT PATH '\$.id', s DOUBLE PATH '\$.s')"
over() { [[ -z ${CELLS:-} || overhead =~ $CELLS ]] || return 0; for _ in $(seq 1000); do echo "$1"; done >"$W/o.sql"; $PIN "$MDBQ" "$ROOT/mysql.sock" bench time "$W/o.sql" 3 | awk -v l="$2" '{ print "overhead", l, $6, $(NF) }'; }
over "SELECT 1" "SELECT-1"
over "SELECT sift_ping(10)" "udf-json-10"
over "SELECT d.n, d.id, d.s FROM JSON_TABLE(sift_ping(10), '\$[*]' COLUMNS $COLS) d" "udf-json_table-10"
over "SELECT d.id, j.s FROM JSON_TABLE(sift_ping(10), '\$[*]' COLUMNS $COLS) j JOIN docs d ON d.id = j.id ORDER BY j.n" "udf-json_table-join-10"
