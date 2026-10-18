#!/usr/bin/env bash
# Latency of sift vs InnoDB FULLTEXT on the loaded Wikipedia table, one client, warm cache, plus the per-call cost of a UDF
# returning JSON. Same session: for every class and kind (top-10, count) the sift cell and the InnoDB cell run back to back
# on the same sample of the trace, and one table at the end compares them. A statement that takes longer than QUERY_TIMEOUT
# is aborted by the server and counted as a timeout (InnoDB OR queries and counts can take seconds); the table reports the
# median of the completed ones and, as a lower bound, the median with every timeout counted as the limit.
#   mariadb/bench/run.sh TRACE        env: SIFT_MDB_ROOT, MDBQ (default $SIFT_BUILD/mdbq),
#                                     QUERIES (queries per class, default 200: evenly spaced sample of the class, the same for
#                                       sift and InnoDB; 1000 = the whole trace),
#                                     QUERY_TIMEOUT (seconds per statement, default 10),
#                                     REPS (passes per sift file, default 2; the last pass is reported),
#                                     INNO_REPS (passes per InnoDB file; default 1 for OR and for counts, whose queries take
#                                       0.1-0.8 s and some far longer, else REPS),
#                                     CELLS (regex on the cell labels, e.g. '^topk or' or 'count': run only those; a table row
#                                       needs both of its cells)
# TRACE: the benchmark trace (id TAB and|or|phrase TAB terms), 1000 queries per class. Database "bench", table docs (id, body)
# with a FULLTEXT index on body, sift indexes "wiki" (the defaults: reads the queue, checks that rows exist) and "wiki_plain"
# (fresh = false, recheck = false: the result of the last drain). The host is measured as it is: the load average is printed at the start and
# at the end. Cell labels: "<topk|count> <and|or|phrase> <sift|sift-udf|sift-plain|inno-natural|inno-boolean|inno>".
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${SIFT_MDB_ROOT:-/tmp/sift-mariadb}
MDBQ=${MDBQ:-${SIFT_BUILD:-/tmp/sift-mariadb-build}/mdbq}
export QUERY_TIMEOUT=${QUERY_TIMEOUT:-10}
REPS=${REPS:-2}
SIFT=
TRACE=$1
W=$(mktemp -d "$ROOT/tmp/bench.XXXXXX")
trap 'rm -rf "$W"' EXIT
load() { cut -d' ' -f1-3 /proc/loadavg; }

# stats: the last pass line of mdbq as "statements completed timeouts median lower-bound-median qps"
stats() { tail -1 | awk '{ for (i = 1; i < NF; i += 2) v[$i] = $(i + 1); print v["statements"], v["completed"], v["timeout"], v["median_us"], v["median_lb_us"], v["qps"] }'; }

# cell KIND CLASS LABEL REPS: runs it and sets RES (empty when CELLS skips it)
cell() {
  RES=
  [[ -z ${CELLS:-} || $3 =~ $CELLS ]] || return 0
  python3 -I "$HERE/mkstmts.py" "$1" "$TRACE" "$2" --limit="${QUERIES:-200}" ${INDEX:+--index=$INDEX} >"$W/s.sql"
  RES=$("$MDBQ" "$ROOT/mysql.sock" bench time "$W/s.sql" "$4" | stats)
  read -r n ok to med lb qps <<<"$RES"
  printf '%-24s completed %s/%s timeout %s median %s us (lower bound %s) qps %s\n' "$3" "$ok" "$n" "$to" "$med" "$lb" "$qps"
}

# sift_cell / inno_cell: the InnoDB cell is compared with the sift cell that ran just before it
sift_cell() { cell "$1" "$2" "$3" "$REPS"; SIFT=$RES; }
inno_cell() {
  local reps=${INNO_REPS:-$REPS}
  [[ -n ${INNO_REPS:-} || ($2 != or && $3 != count*) ]] || reps=1
  cell "$1" "$2" "$3" "$reps"
  [[ -z $SIFT || -z $RES ]] || printf '%s\t%s\t%s\n' "$3" "$SIFT" "$RES" >>"$W/table"
}

# table: LABEL TAB sift stats TAB InnoDB stats
table() {
  awk -F'\t' '
    BEGIN { printf "%-24s %10s %12s %16s %26s %12s\n", "class", "sift us", "InnoDB us", "InnoDB timeouts", "InnoDB us (timeouts=limit)", "InnoDB/sift" }
    { split($2, s, " "); split($3, i, " ")
      ratio = s[5] > 0 && i[5] >= 0 ? sprintf("%s%.0fx", i[3] > 0 ? ">=" : "", i[5] / s[5]) : "-"
      printf "%-24s %10s %12s %16s %26s %12s\n", $1, s[4], i[4], i[3] "/" i[1] " timeout", i[5], ratio }' "$W/table"
}

echo "# load average at start: $(load), timeout ${QUERY_TIMEOUT} s, ${QUERIES:-200} queries per class"
for c in and or phrase; do
  sift_cell sift-topk-dt "$c" "topk $c sift"
  cell sift-raw "$c" "topk $c sift-udf" "$REPS"
  INDEX=wiki_plain cell sift-topk-dt "$c" "topk $c sift-plain" "$REPS"
  if [[ $c == or ]]; then
    inno_cell inno-topk "$c" "topk $c inno-natural"
    inno_cell inno-topk-bool "$c" "topk $c inno-boolean"
  else
    inno_cell inno-topk "$c" "topk $c inno-boolean"
  fi
  sift_cell sift-count "$c" "count $c sift"
  INDEX=wiki_plain cell sift-count "$c" "count $c sift-plain" "$REPS"
  inno_cell inno-count "$c" "count $c inno"
done

# per-call cost: the protocol floor, a UDF returning JSON, JSON_TABLE over it, and the join an application adds
COLS="(n FOR ORDINALITY, id BIGINT PATH '\$.id', s DOUBLE PATH '\$.s')"
over() {
  [[ -z ${CELLS:-} || overhead =~ $CELLS ]] || return 0
  for _ in $(seq 1000); do echo "$1"; done >"$W/o.sql"
  "$MDBQ" "$ROOT/mysql.sock" bench time "$W/o.sql" 3 | stats | awk -v l="$2" '{ print "overhead", l, $4, $6 }'
}
over "SELECT 1" "SELECT-1"
over "SELECT sift_ping(10)" "udf-json-10"
over "SELECT d.n, d.id, d.s FROM JSON_TABLE(sift_ping(10), '\$[*]' COLUMNS $COLS) d" "udf-json_table-10"
over "SELECT d.id, j.s FROM JSON_TABLE(sift_ping(10), '\$[*]' COLUMNS $COLS) j JOIN docs d ON d.id = j.id ORDER BY j.n" "udf-json_table-join-10"

echo
[[ ! -s $W/table ]] || table
echo "# medians in microseconds over the completed statements; ratio = InnoDB median with timeouts at the limit / sift median (>= when InnoDB timed out)"
echo "# load average at end: $(load)"
