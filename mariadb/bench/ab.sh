#!/usr/bin/env bash
# Two builds of libsift_udf.so against the same server and corpus, interleaved: the regression gate of a change to the library.
# For each round the server is restarted with build A, then with build B (the order alternates), each build has its own indexes
# (ab_a, ab_a_plain, ab_b, ab_b_plain: the default index and one with fresh=false, recheck=false) of the table docs, and the cells of
# bench/run.sh that only involve sift run on them, then the index is rebuilt (sift_rebuild) for the build time. At the end every cell
# and the build are reported as mean +- standard error over the rounds for A and B, and B / A of the paired rounds.
#
#   mariadb/bench/ab.sh TRACE BUILD_A BUILD_B [ROUNDS]
# BUILD_x: a directory with libsift_udf.so. The server of mariadb/server.sh must be stopped, SIFT_MDB_ROOT holds the loaded table
# (test.sh with STEPS="build load" KEEP=1). env: QUERIES (queries per class, default 200), REPS (passes per cell, default 2),
# MDBQ (default $SIFT_BUILD/mdbq of BUILD_A).
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SRV=$HERE/server.sh
export SIFT_MDB_ROOT=${SIFT_MDB_ROOT:-/tmp/sift-mariadb}
TRACE=$1 A=$2 B=$3 ROUNDS=${4:-6}
MDBQ=${MDBQ:-$A/mdbq}
W=$(mktemp -d "$SIFT_MDB_ROOT/tmp/ab.XXXXXX")
trap '"$SRV" stop; rm -rf "$W"' EXIT
sql() { "$SRV" sql -N -B bench -e "$1"; }
now() { date +%s.%N; }
opts='{"dpp":32,"fresh":false,"recheck":false}'

# cell VARIANT LABEL KIND CLASS INDEX: median and qps of the last pass
cell() {
  python3 -I "$HERE/bench/mkstmts.py" "$3" "$TRACE" "$4" --limit="${QUERIES:-200}" --index="$5" >"$W/s.sql"
  "$MDBQ" "$SIFT_MDB_ROOT/mysql.sock" bench time "$W/s.sql" "${REPS:-2}" | tail -1 |
    awk -v r="$ROUND" -v v="$1" -v l="$2" '{ for (i = 1; i < NF; i += 2) x[$i] = $(i + 1); printf "%d\t%s\t%s\t%s\t%s\n", r, v, l, x["median_us"], x["qps"] }' >>"$W/cells.tsv"
}

measure() { # measure VARIANT BUILD
  local v=$1 i=ab_$1 c
  SIFT_BUILD=$2 "$SRV" restart >/dev/null
  echo "# round $ROUND $v: load $(cut -d' ' -f1-3 /proc/loadavg)"
  sql "SELECT COUNT(*) FROM docs" >/dev/null
  for c in and or phrase; do
    cell "$v" "topk $c" sift-topk-dt "$c" "$i"
    cell "$v" "topk $c udf-only" sift-raw "$c" "$i"
    cell "$v" "topk $c plain" sift-topk-dt "$c" "${i}_plain"
    cell "$v" "count $c" sift-count "$c" "$i"
    cell "$v" "count $c plain" sift-count "$c" "${i}_plain"
  done
  local t0; t0=$(now)
  sql "CALL sift_rebuild('$i')" >/dev/null
  awk -v r="$ROUND" -v v="$v" -v t="$(now)" -v t0="$t0" -v b="$(du -sb "$SIFT_MDB_ROOT/data/.sift/$i" | cut -f1)" \
    'BEGIN { printf "%d\t%s\tbuild (s)\t%.2f\t\n%d\t%s\tsize (MB)\t%.1f\t\n", r, v, t - t0, r, v, b / 1e6 }' >>"$W/cells.tsv"
}

for v in a b; do   # indexes of each build, made by that build
  [[ $v == a ]] && dir=$A || dir=$B
  SIFT_BUILD=$dir "$SRV" restart >/dev/null
  sql "CALL sift_attach('ab_$v', 'docs', 'id', 't.body', 32); CALL sift_sync('ab_$v');
       CALL sift_attach_opts('ab_${v}_plain', 'docs', 'id', 't.body', '$opts'); CALL sift_sync('ab_${v}_plain')" >/dev/null
done
for ROUND in $(seq "$ROUNDS"); do
  if ((ROUND % 2)); then measure a "$A"; measure b "$B"; else measure b "$B"; measure a "$A"; fi
done

python3 -I - "$W/cells.tsv" <<'EOF'
import math, sys
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1])]
cells, order = {}, []
for r, v, label, med, qps in rows:
    if label not in order:
        order.append(label)
    cells.setdefault((label, v), {})[int(r)] = float(med)

def mean_se(xs):
    n = len(xs); m = sum(xs) / n
    return m, (math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1) / n) if n > 1 else 0.0)

print('%-22s %20s %20s %24s' % ('cell (median us)', 'A', 'B', 'B / A (paired rounds)'))
for label in order:
    a, b = cells[(label, 'a')], cells[(label, 'b')]
    ma, sa = mean_se(list(a.values())); mb, sb = mean_se(list(b.values()))
    ratio = [b[r] / a[r] for r in a if r in b and a[r] > 0]
    mr, sr = mean_se(ratio)
    print('%-22s %11.1f +- %-6.1f %11.1f +- %-6.1f %14.3f +- %.3f' % (label, ma, sa, mb, sb, mr, sr))
EOF
