# Sourced by every bench script. Override any variable from the environment.
set -euo pipefail
export LC_ALL=C

BENCH=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${SIFT_BENCH_ROOT:-/tmp/sift-bench}
TIER=${TIER:-Q}
declare -A TIER_PORT=([Q]=56040 [F]=56050)
declare -A TIER_SHBUF=([Q]=1GB [F]=4GB)
declare -A TIER_MWM=([Q]=512MB [F]=2GB)
PGDATA=${PGDATA:-$ROOT/pg/$TIER}
SOCK=$ROOT/run
CACHE=$ROOT/cache
DATA=$ROOT/data
BM25_ROOT=$ROOT/bm25root
PARADEDB_ROOT=$ROOT/engines/paradedb/root
OUT=${OUT:-$BENCH/out/$TIER}
LOCK=/tmp/sift-perf.lock
export PGHOST=$SOCK PGPORT=${PORT:-${TIER_PORT[$TIER]}} PGDATABASE=${DB:-bench} PGUSER=${PGUSER:-$USER}

q()  { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
log() { printf '[%s %s] %s\n' "$(date +%T)" "$TIER" "$*" >&2; }
# Timed sections must not overlap with other agents' timings.
locked() { ( flock 9; "$@" ) 9>>"$LOCK"; }
now_ms() { date +%s%3N; }
engine_db() { echo "e_${1//-/_}"; }

load_engine() { ENGINE=$1; . "$BENCH/engines/$1.sh"; }
engine_available() {
  [[ -z $REQUIRES ]] || [[ $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_available_extensions WHERE name = '$REQUIRES'") == 1 ]]
}
clone_db() {
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $1 WITH (FORCE)" -c "CREATE DATABASE $1 TEMPLATE bench STRATEGY FILE_COPY"
  export DB=$1 PGDATABASE=$1
  q -f "$BENCH/sql/harness.sql"
}
# Untimed engine database for correctness checks, reused across calls unless REBUILD=1.
check_db() {
  local db=o_$ENGINE
  if [[ -z ${REBUILD:-} && $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_database WHERE datname = '$db'") == 1 ]]; then
    export DB=$db PGDATABASE=$db
  else
    clone_db "$db"
    q -c "$SETUP" -c "$INDEX" -c "VACUUM (ANALYZE) docs" 2>/dev/null
  fi
  q -c "$RENDER"
}
# Every engine server runs with the same budget (TIN: 8 vCPUs, 32 GB); load generators run outside it.
limited() { systemd-run --user --scope --quiet -p CPUQuota="${CPU_QUOTA:-800%}" -p MemoryMax="${MEM_MAX:-32G}" nice "$@"; }
