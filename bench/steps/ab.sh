#!/usr/bin/env bash
# Interleaved A/B of sift, sift with phrase_index = pairs, ParadeDB pg_search, pg_textsearch and rum on one corpus.
#
#   bench/steps/ab.sh quick          Q tier (150k articles), 3 s per cell, 3-6 rounds
#   bench/steps/ab.sh full           F tier (218k articles), 8 s per cell, 4-6 rounds
#   bench/steps/ab.sh cold [quick]   cold-cache execution times (index and heap evicted before every query)
#
# Every engine database is built once (BUILD_ONLY in run.sh), then each round runs every class on
# every engine, one cell at a time with the engine order rotated, so load drifts hit all engines alike.
# Rounds stop once every cell's spread is under TOL (default 15%) after the minimum, or at MAXR.
# Only compact tables are kept: $DEST (default $RESULTS/<mode>-<time>)/{cells,builds,summary}.tsv and
# machine.txt; the scratch cluster is removed on exit (KEEP=1 keeps it). The corpus dump stays in $CACHE.
# Engines whose extension is not installed are skipped with a message; a class an engine cannot render
# is reported as NA. Env: ENGINES CLASSES COLD (cold classes) CLIENTS SECS WARM MINR MAXR TOL NQ (cold
# queries per class) RESULTS DEST.
# Needs bench/bench setup (the engines, the articles dataset).
set -euo pipefail
mode=${1:-quick}; cold=
[[ $mode == cold ]] && { cold=1; mode=${2:-full}; }
case $mode in
  quick) export TIER=Q; SECS=${SECS:-3} WARM=${WARM:-1} MINR=${MINR:-3} MAXR=${MAXR:-6} ;;
  full)  export TIER=F; SECS=${SECS:-8} WARM=${WARM:-2} MINR=${MINR:-4} MAXR=${MAXR:-6} ;;
  *) sed -n '2,6p' "$0"; exit 1 ;;
esac
. "$(dirname "$0")/lib.sh"
ENGINES=${ENGINES:-sift sift_pairs pg_search pg_textsearch rum}
CLASSES=${CLASSES:-and or phrase mixed count filter join}
CLIENTS=${CLIENTS:-8} THREADS=${THREADS:-4} TOL=${TOL:-0.15} NQ=${NQ:-12}
RESULTS=${RESULTS:-$BENCH/out/ab}
OUT=$ROOT/ab/$TIER; rm -rf "$OUT"; mkdir -p "$OUT" "$RESULTS"   # builds.tsv from run.sh lands in the scratch directory
DEST=${DEST:-$RESULTS/${cold:+cold-}$mode-$(date +%m%d-%H%M)}; mkdir -p "$DEST"
export OUT

cleanup() { [[ ${KEEP:-} ]] || { "$STEPS/cluster.sh" destroy >/dev/null 2>&1 || true; rm -rf "$ROOT/ab"; }; }
trap cleanup EXIT

"$STEPS/cluster.sh" init >/dev/null
"$STEPS/cluster.sh" start >/dev/null
ENGINES=$(available_engines "$ENGINES" | tr '\n' ' ')
"$STEPS/machine.sh" >"$DEST/machine.txt"
[[ -f $DUMP ]] || "$STEPS/corpus.sh" load >/dev/null
db_exists bench || "$STEPS/corpus.sh" restore >/dev/null
for e in $ENGINES; do BUILD_ONLY=1 "$STEPS/run.sh" "$e" >/dev/null 2>&1 || { echo "build of $e failed" >&2; exit 1; }; done
cp "$OUT/builds.tsv" "$DEST/builds.tsv"
log "built $ENGINES: $(awk '{printf "%s %.1fs %.0fMB; ", $2, $5, $6 / 1048576}' "$OUT/builds.tsv")"

if [[ $cold ]]; then
  printf 'class\tengine\tp50_ms\tmax_ms\n' >"$DEST/summary.tsv"
  for e in $ENGINES; do PGDATABASE=$(engine_db "$e") q -c "CREATE EXTENSION IF NOT EXISTS pg_buffercache" >/dev/null; done
  for c in ${COLD:-phrase count and}; do for e in $ENGINES; do
    PGDATABASE=$(engine_db "$e"); export PGDATABASE
    t=$(for i in $(seq "$NQ"); do
      stmt=$(q -c "SELECT stmt FROM stmts WHERE wl = '$c' AND n = $i")
      evict_table docs
      explain_ms "$stmt"
    done | median_max)
    printf '%s\t%s\t%s\n' "$c" "$e" "$t" | tee -a "$DEST/summary.tsv"
  done; done
  exit 0
fi

# one pgbench script per class and engine database (the statements were rendered per engine)
for e in $ENGINES; do for c in $CLASSES; do
  n=$(PGDATABASE=$(engine_db "$e") q -c "SELECT count(*) FROM stmts WHERE wl = '$c'")
  (( n > 0 )) || { rm -f "$OUT/$e-$c.pgb"; continue; }   # the engine cannot render this class
  printf '\\set i random(1, %d)\nSELECT stmt FROM stmts WHERE wl = '"'"'%s'"'"' AND n = :i \\gset\n:stmt\n' "$n" "$c" >"$OUT/$e-$c.pgb"
done; done
cell() { # engine class secs -> qps (NA: the engine cannot run the class; 0: a client failed, e.g. the statement timeout)
  [[ -f $OUT/$1-$2.pgb ]] || { echo NA; return; }
  local o
  o=$(PGOPTIONS="-c statement_timeout=${TIMEOUT:-60s}" pgbench -n -c "$CLIENTS" -j "$THREADS" -T "$3" -f "$OUT/$1-$2.pgb" "$(engine_db "$1")" 2>&1) || { echo 0; return; }
  grep -oP 'tps = \K[0-9.]+(?= \(without)' <<<"$o" || echo 0
}
stable() { spread_ok "$DEST/cells.tsv" 3,4 5 -1; }   # class, engine; qps; a failed cell (0) is never stable

printf 'round\tload\tclass\tengine\tqps\n' >"$DEST/cells.tsv"
round() {
  local c e
  for c in $CLASSES; do for e in $(rotated "$1" $ENGINES); do
    cell "$e" "$c" "$WARM" >/dev/null
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$(loadavg)" "$c" "$e" "$(cell "$e" "$c" "$SECS")" >>"$DEST/cells.tsv"
  done; done
}
rounds round

python3 -I - "$DEST" "$ENGINES" "$CLASSES" <<'PY'
import statistics as st, sys
dest, engines, classes = sys.argv[1], sys.argv[2].split(), sys.argv[3].split()
cells = [l.rstrip('\n').split('\t') for l in open(dest + '/cells.tsv')][1:]
rounds = sorted({int(c[0]) for c in cells})
load = {r: [float(c[1]) for c in cells if int(c[0]) == r] for r in rounds}
def median(c, e):
    v = [float(x[4]) for x in cells if x[2] == c and x[3] == e and x[4] != 'NA']
    return st.median(v) if v else None
med = {(c, e): median(c, e) for c in classes for e in engines}
num = lambda v: 'NA' if v is None else '%.0f' % v
ratio = lambda a, b: 'NA' if a is None or not b else '%.2f' % (a / b)
ref = 'pg_search' if 'pg_search' in engines else engines[-1]
with open(dest + '/summary.tsv', 'w') as f:
    f.write('class\t' + '\t'.join(engines) + '\t' + '\t'.join(e + '/' + ref for e in engines if e != ref) + '\n')
    for c in classes:
        f.write(c + '\t' + '\t'.join(num(med[c, e]) for e in engines) + '\t' +
                '\t'.join(ratio(med[c, e], med[c, ref]) for e in engines if e != ref) + '\n')
    f.write('load\t' + ' '.join('r%d %.1f-%.1f' % (r, min(load[r]), max(load[r])) for r in rounds) + '\n')
print(open(dest + '/summary.tsv').read())
PY
log "results in $DEST"
