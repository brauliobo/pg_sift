#!/usr/bin/env bash
# Usage: bench/bench cases [--cases ID,ID] [--engines pg_duckdb] [--rows N] [--rounds R] [--runs N] [--cpus N] [--docs] [--out FILE] [--no-wait] [--keep]
#        bench/bench cases --list | --check [--strict]
#        bench/bench cases --discover [--shapes FILE] [--fuzz SEEDS] [--results FILE] [--min-gain 2] [--out FILE]   candidate cases from the corpora (steps/cases_discover.py)                                                                       (bench/steps/cases.sh, called by bench/bench)
# The benchmark cases of the 3.3 optimizations (bench/cases.tsv, bench/cases/*.sql, steps/cases.py): each case is timed on pg_sift (the planner's own choice, the plan asserted from
# EXPLAIN), on PostgreSQL with sift.enable_agg = off (the same table and statement), on the sift variants of its row and, with --engines pg_duckdb, on a pg_duckdb container that holds a
# plain copy of the rows; every answer is compared with PostgreSQL's. A private cluster (scripts/pgtest.sh, production build) under a CPU quota of --cpus (default 1: one backend, no
# parallel workers, how the CHANGELOG numbers were taken; 3 is the budget of the store benchmark, where a parallel normal plan can beat the scan on the planner's cost) and a memory cap; the timed part holds the
# perf lock. Small by default (--rows 200000); the published size is `bench/bench cases --docs --rows 1000000` (docs/results/cases.tsv and the generated blocks).
#   --docs      write docs/results/cases.tsv (only the rows of the cases run) and the blocks of README.md and docs/BENCHMARKS.md
#   --no-wait   do not wait for a quiet host (the rows say NOISY when it was busy)       --keep   leave the cluster and the container up
# Settings: CASES_TMP (cluster directory, default /var/tmp/sift-cases), CASES_PORT (56100-56299, default 56262; pg_duckdb 56263), CASES_MEM (memory cap, default 8G).
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=cases

ROWS=200000 ROUNDS=3 RUNS=5 CPUS=1 DOCS='' KEEP='' ENGINES='' PY=() DISC='' DISCOVER=()
while (( $# )); do
  case $1 in
    --list) exec python3 -I "$STEPS/cases.py" list ;;
    --discover) DISC=1; shift; DISCOVER=("$@"); break ;;
    --check) shift; exec "$REPO/scripts/check-bench-cases.sh" "$@" ;;
    --rows) ROWS=$2; shift ;;
    --rounds) ROUNDS=$2; shift ;;
    --runs) RUNS=$2; shift ;;
    --cpus) CPUS=$2; shift ;;
    --engines) ENGINES=$2; shift ;;
    --docs) DOCS=1 ;;
    --keep) KEEP=1 ;;
    --cases|--out|--wait) PY+=("$1" "$2"); shift ;;
    --no-wait) PY+=("$1") ;;
    *) sed -n '2,11p' "$0"; exit 1 ;;
  esac
  shift
done
need psql python3 flock
[[ $CPUS =~ ^[1-9][0-9]*$ && $ROWS =~ ^[1-9][0-9]*$ ]] || die "--cpus and --rows take whole numbers"

export SIFT_TMP=${CASES_TMP:-/var/tmp/sift-cases} SIFT_PORT=${CASES_PORT:-56262} SIFT_TESTING= CASES_TMP=${CASES_TMP:-/var/tmp/sift-cases} CASES_WORKERS=$((CPUS - 1))
export CAP_MEM=${CASES_MEM:-8G} CAP_CPU=$((CPUS * 100))%
PGTEST=$REPO/scripts/pgtest.sh
DUCK=sift-cases-duck DUCK_PORT=$((SIFT_PORT + 1))
SETTINGS=(shared_buffers=1GB effective_cache_size=3GB work_mem=64MB maintenance_work_mem=256MB max_wal_size=4GB jit=off max_worker_processes=16 max_parallel_workers=16
          parallel_setup_cost=100 parallel_tuple_cost=0.01 min_parallel_table_scan_size=1MB min_parallel_index_scan_size=64kB checkpoint_timeout=30min)

cleanup() {
  [[ -n $KEEP ]] || { "$PGTEST" destroy >/dev/null 2>&1 || true; docker rm -fv "$DUCK" >/dev/null 2>&1 || true; }
}
trap cleanup EXIT

mkdir -p "$SIFT_TMP"
"$PGTEST" install >/dev/null
capped "$PGTEST" start
eval "$("$PGTEST" env)"
for s in "${SETTINGS[@]}"; do psql -X -q -U postgres -d postgres -c "ALTER SYSTEM SET ${s%%=*} = '${s#*=}'"; done
psql -X -q -U postgres -d postgres -c "DROP DATABASE IF EXISTS cases WITH (FORCE)" -c "CREATE DATABASE cases"
"$PGTEST" stop
capped "$PGTEST" start
eval "$("$PGTEST" env)"
psql -X -q -U postgres -d cases -c "CREATE EXTENSION pg_sift"

if [[ ,$ENGINES, == *,pg_duckdb,* ]]; then
  need docker
  ports_free "$DUCK_PORT" 1 || exit 1
  pull_missing "$STORE_DUCKDB_IMAGE"
  mapfile -t CAPS < <(docker_caps "$CPUS" 8g)
  docker rm -fv "$DUCK" >/dev/null 2>&1 || true
  docker run -d --name "$DUCK" "${CAPS[@]}" --shm-size 1g -p "127.0.0.1:$DUCK_PORT:5432" -e POSTGRES_HOST_AUTH_METHOD=trust "$STORE_DUCKDB_IMAGE" postgres "${PG_FLAGS[@]}" \
    -c max_parallel_workers_per_gather=$((CPUS - 1)) -c duckdb.threads="$CPUS" -c duckdb.threads_for_postgres_scan="$CPUS" -c duckdb.max_workers_per_postgres_scan=$((CPUS - 1)) >/dev/null
  for _ in $(seq 60); do psql -X -qAt -h 127.0.0.1 -p "$DUCK_PORT" -U postgres -d postgres -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 1; done
  psql -X -q -h 127.0.0.1 -p "$DUCK_PORT" -U postgres -d postgres -c "DROP DATABASE IF EXISTS bench WITH (FORCE)" -c "CREATE DATABASE bench"
  PY+=(--duck-port "$DUCK_PORT" --engines "$ENGINES")
fi

if [[ -n $DISC ]]; then locked python3 -I "$STEPS/cases_discover.py" ${DISCOVER[@]+"${DISCOVER[@]}"}; exit; fi
locked python3 -I "$STEPS/cases.py" run --rows "$ROWS" --rounds "$ROUNDS" --runs "$RUNS" ${DOCS:+--docs} ${PY[@]+"${PY[@]}"}
