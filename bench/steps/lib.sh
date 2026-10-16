# Sourced by every bench script. Override any variable from the environment.
set -euo pipefail
export LC_ALL=C

STEPS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)   # the internal steps that bench/bench runs
BENCH=$(dirname "$STEPS")
ALL_ENGINES=$(awk -F'\t' '!/^#/ {printf "%s%s", sep, $1; sep = " "}' "$BENCH/engines.tsv")   # the A/B engines, in table order
REPO=$(dirname "$BENCH")
. "$BENCH/versions.env"
command -v pg_config >/dev/null && PATH=$(pg_config --bindir):$PATH   # Debian keeps the server tools off PATH
ROOT=${SIFT_BENCH_ROOT:-/tmp/sift-bench}
TIER=${TIER:-Q}
declare -A TIER_PORT=([Q]=56040 [F]=56050)
declare -A TIER_DOCS=([Q]=${Q_DOCS:-150000} [F]=ALL)   # articles per tier; the cached dump carries the count
declare -A TIER_SHBUF=([Q]=1GB [F]=4GB)
declare -A TIER_MWM=([Q]=512MB [F]=2GB)
PGDATA=${PGDATA:-$ROOT/pg/$TIER}
SOCK=$ROOT/run
CACHE=$ROOT/cache
DUMP=$CACHE/sift-$TIER-${TIER_DOCS[$TIER]}.dump
DATA=$ROOT/data
ENG=$ROOT/engines                            # engines/<name>/{dl,root}: downloads, and the installed (DESTDIR) or unpacked extension
SIFT_ROOT=${SIFT_ROOT:-$ENG/sift/root}
JOBS=${JOBS:-4}                              # parallel jobs of the builds in bench/steps/fetch.sh
OUT=${OUT:-$BENCH/out/$TIER}
LOCK=/tmp/sift-perf.lock
export PGHOST=$SOCK PGPORT=${PORT:-${TIER_PORT[$TIER]}} PGDATABASE=${DB:-bench} PGUSER=${PGUSER:-$USER}

# Where the cluster looks for extensions (bench/steps/cluster.sh): every engine root first, then the system.
SHAREDIR=$(pg_config --sharedir) PKGLIBDIR=$(pg_config --pkglibdir)
engine_paths() { { find "$ENG"/*/root -name "$1" -printf '%h\n' 2>/dev/null || true; } | sort -u; }   # directories holding files named $1
EXT_SHARE=$(engine_paths '*.control' | sed 's,/extension$,,' | paste -sd:)
EXT_LIB=$(engine_paths '*.so' | paste -sd:)

q()  { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
log() { printf '[%s %s] %s\n' "$(date +%T)" "$TIER" "$*" >&2; }
loadavg() { cut -d' ' -f1 /proc/loadavg; }
# Timed sections must not overlap with other agents' timings.
locked() { ( flock 9; "$@" ) 9>>"$LOCK"; }
now_ms() { date +%s%3N; }
elapsed_s() { awk -v a="$1" -v b="$2" 'BEGIN {printf "%.1f", (b - a) / 1000}'; }   # ms timestamps -> seconds
engine_db() { echo "e_${1//-/_}"; }
db_exists() { [[ $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_database WHERE datname = '$1'") == 1 ]]; }
drop_db() { PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $1 WITH (FORCE)"; }

# An adapter (bench/engines/<name>.sh) sets REQUIRES (extension), SETUP and INDEX (SQL), and either PRED and RANK
# (SQL expressions over `mode` and `terms` that render the WHERE condition and the ORDER BY of a query, NULL when
# the engine cannot answer it; SOURCE: the FROM item of ranked statements when it is not `docs WHERE PRED`; HELPERS: SQL
# they use) or, for engines outside Postgres, RENDER and the x_* functions.
render_sql() {
  echo "${HELPERS:-}"
  printf 'CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS $b$ SELECT %s $b$;\n' "$PRED"
  printf 'CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS $b$ SELECT %s $b$;\n' "$RANK"
  printf 'CREATE OR REPLACE FUNCTION bench_source(mode text, terms text[]) RETURNS text LANGUAGE sql AS $b$ SELECT %s $b$;\n' "${SOURCE:-NULL::text}"
}
load_engine() {
  ENGINE=$1; REQUIRES=''; SETUP=''; INDEX=''; PRED=''; RANK=''; SOURCE=''; HELPERS=''; RENDER=''
  . "$BENCH/engines/$1.sh"
  [[ -n $RENDER ]] || RENDER=$(render_sql)
}
# the step that times an engine's workloads: Postgres adapters through pgbench, external ones through xbench
runner() { if grep -q '^EXTERNAL=' "$BENCH/engines/$1.sh"; then echo xrun.sh; else echo run.sh; fi; }
engine_available() {
  [[ -z $REQUIRES ]] || [[ $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_available_extensions WHERE name = '$REQUIRES'") == 1 ]]
}
# Prints the engines of $1 whose extension is installed; says why the others are left out (stderr).
available_engines() {
  local e
  for e in $1; do
    ( load_engine "$e"; engine_available ) && echo "$e" || log "skipping $e: its extension is not installed (bench/bench setup)" >&2
  done
}
clone_db() {
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $1 WITH (FORCE)" -c "CREATE DATABASE $1 TEMPLATE bench STRATEGY FILE_COPY"
  export DB=$1 PGDATABASE=$1
  q -f "$BENCH/sql/harness.sql"
}
# Untimed engine database for correctness checks, reused across calls unless REBUILD=1.
check_db() {
  local db=o_$ENGINE
  if [[ -z ${REBUILD:-} ]] && db_exists "$db"; then
    export DB=$db PGDATABASE=$db
  else
    clone_db "$db"
    q -c "$SETUP" -c "$INDEX" -c "VACUUM (ANALYZE) docs" 2>/dev/null
  fi
  q -c "$RENDER"
}

# --- interleaved rounds (bench/steps/ab.sh, places.sh)
# rounds FN: runs FN ROUND for MAXR rounds, stopping after MINR once the caller's `stable` holds.
rounds() {
  local r
  for r in $(seq "$MAXR"); do
    "$1" "$r"
    log "round $r done, load $(loadavg)"
    (( r >= MINR )) && stable && break
  done
  return 0
}
rotated() { local r=$1 n=$(( $# - 1 )) k; shift; local e=("$@"); for ((k = 0; k < n; k++)); do echo "${e[$(( (k + r) % n ))]}"; done; }   # engines in the order of round r
# spread_ok FILE KEYCOLS VALCOL FLOOR: for every cell (rows of the tab separated FILE by the columns KEYCOLS, e.g. 3,4),
# max-min of the numbers in VALCOL over the rounds is within TOL of the max; cells whose max is <= FLOOR are ignored.
spread_ok() {
  awk -F'\t' -v tol="$TOL" -v kc="$2" -v vc="$3" -v floor="$4" 'BEGIN { nk = split(kc, K, ",") }
    NR > 1 && $vc ~ /^[0-9.]+$/ { k = ""; for (i = 1; i <= nk; i++) k = k SUBSEP $(K[i]); n[k]++; v[k, n[k]] = $vc }
    END { for (k in n) { lo = hi = v[k, 1]; for (i = 2; i <= n[k]; i++) { if (v[k, i] < lo) lo = v[k, i]; if (v[k, i] > hi) hi = v[k, i] }
          if (hi > floor && (hi == 0 || (hi - lo) / hi > tol)) exit 1 } }' "$1"
}

# --- cold cache: empty the relations from shared buffers and the OS page cache, then time one statement
# evict SQL: SQL returns the relations (regclass or oid, one per row)
evict() {
  q -c "SELECT pg_buffercache_evict_relation(c) FROM ($1) x(c)" >/dev/null
  # shellcheck disable=SC2046  # one argument per relation file
  python3 -I - "$PGDATA" $(q -c "SELECT pg_relation_filepath(c) FROM ($1) x(c)") <<'PY'
import glob, os, sys
for rel in sys.argv[2:]:
    for f in glob.glob(os.path.join(sys.argv[1], rel) + '*'):
        fd = os.open(f, os.O_RDONLY); os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED); os.close(fd)
PY
}
evict_table() { # the heap, TOAST table and every index of table $1
  evict "SELECT oid FROM pg_class WHERE oid = '$1'::regclass OR oid = (SELECT reltoastrelid FROM pg_class WHERE oid = '$1'::regclass)
         UNION SELECT indexrelid FROM pg_index WHERE indrelid = '$1'::regclass"
}
explain_ms() { # STATEMENT [psql args]: execution time in ms
  echo "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, COSTS OFF) $1" | psql -X -qAt "${@:2}" | sed -n 's/^Execution Time: \([0-9.]*\) ms/\1/p'
}
median_max() { sort -n | awk '{v[NR] = $1} END {printf "%.2f\t%.2f", v[int((NR + 1) / 2)], v[NR]}'; }   # of numbers on stdin

# Every engine server runs with the same budget (8 vCPUs, 32 GB, as a cgroup scope); load generators run
# outside it. Without a systemd user session the budget is not enforced (bench/steps/machine.sh says which).
if [[ -z ${BUDGET_ENFORCED:-} ]]; then
  systemd-run --user --scope --quiet true 2>/dev/null && BUDGET_ENFORCED=yes || BUDGET_ENFORCED=no
  export BUDGET_ENFORCED
fi
limited() {
  if [[ $BUDGET_ENFORCED == yes ]]; then
    systemd-run --user --scope --quiet -p CPUQuota="${CPU_QUOTA:-800%}" -p MemoryMax="${MEM_MAX:-32G}" nice "$@"
  else
    nice "$@"
  fi
}
