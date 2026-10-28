#!/usr/bin/env bash
# Usage: bench/bench cases [--cases ID,ID] [--engines pg_duckdb] [--rows N] [--scales 1,10,100] [--work-mem 4GB] [--timeout S] [--io cold|warm|off] [--rounds R] [--runs N] [--cpus N] [--docs] [--out FILE] [--times FILE] [--lane-budget S] [--no-share] [--any-plan] [--no-wait] [--keep]
#        bench/bench cases --list | --plan [--rows N] [--scales L] [--cases ID,ID]   (the points, builds, disk and time a run needs, nothing started)
#        bench/bench cases --discover [--shapes FILE] [--fuzz SEEDS] [--results FILE] [--min-gain 2] [--out FILE]   candidate cases from the corpora (steps/cases_discover.py)                                                                       (bench/steps/cases.sh, called by bench/bench)
# The benchmark cases (bench/cases.tsv, bench/cases/*.sql, steps/cases.py): each case is timed on pg_sift (the planner's own choice, the plan asserted from
# EXPLAIN), on PostgreSQL with sift.enable_agg = off (the same table and statement), on the sift variants of its row and, with --engines pg_duckdb, on a pg_duckdb container that holds a
# plain copy of the rows; every answer is compared with PostgreSQL's. A private cluster (scripts/pgtest.sh, release build) under a CPU quota of --cpus (default 1: one backend, no
# parallel workers, how the CHANGELOG numbers were taken; 3 is the budget of the store benchmark, where a parallel normal plan can beat the scan on the planner's cost) and a memory cap; the timed part holds the
# perf lock. Small by default (--rows 200000); the published size is `bench/bench cases --docs --rows 1000000`.
#   --scales L  every case at --rows times each number of L, the planner's plan at each size (a flip is a finding, not a failure): bench/results/cases-scale.tsv and the block "Benchmark cases, by size"
#               with its crossover per case; 1,10,100 of 200,000 is 20 million orders (CASES_MEM, --timeout 1800 and a large host: `bench/bench refresh --stages scale --budget 0`)
#   --docs      write bench/results/cases.tsv (only the rows of the cases run) and the blocks of README.md and docs/BENCHMARKS.md
#   --no-wait   do not wait for a quiet host (the rows say NOISY when it was busy)       --keep   leave the cluster and the container up
# Settings: CASES_TMP (cluster directory, default /dev/shm/sift-cases: tmpfs, the clusters are scratch and run with fsync off; a run that needs more than 4 GB points it elsewhere), CASES_PORT (56100-56299, default 56262; pg_duckdb 56263), CASES_MEM (memory cap, default 8G).
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=cases

ROWS=200000 ROUNDS=3 RUNS=5 CPUS=1 DOCS='' KEEP='' ENGINES='' PY=() DISC='' DISCOVER=()
while (( $# )); do
  case $1 in
    --list) exec python3 -I "$STEPS/cases.py" list ;;
    --plan) shift; exec python3 -I "$STEPS/cases.py" plan "$@" ;;
    --discover) DISC=1; shift; DISCOVER=("$@"); break ;;
    --rows) ROWS=$2; shift ;;
    --rounds) ROUNDS=$2; shift ;;
    --runs) RUNS=$2; shift ;;
    --cpus) CPUS=$2; shift ;;
    --engines) ENGINES=$2; shift ;;
    --docs) DOCS=1 ;;
    --keep) KEEP=1 ;;
    --cases|--out|--wait|--scales|--timeout|--work-mem|--times|--lane-budget|--io) PY+=("$1" "$2"); shift ;;
    --no-wait|--no-share|--any-plan) PY+=("$1") ;;
    *) sed -n '2,11p' "$0"; exit 1 ;;
  esac
  shift
done
need psql python3 flock
[[ $CPUS =~ ^[1-9][0-9]*$ && $ROWS =~ ^[1-9][0-9]*$ ]] || die "--cpus and --rows take whole numbers"

export SIFT_TMP=${CASES_TMP:-/dev/shm/sift-cases} SIFT_PORT=${CASES_PORT:-56262} SIFT_TESTING= CASES_TMP=${CASES_TMP:-/dev/shm/sift-cases} CASES_WORKERS=$((CPUS - 1))
export CAP_MEM=${CASES_MEM:-8G} CAP_CPU=$((CPUS * 100))%
PGTEST=$REPO/scripts/pgtest.sh
DUCK=sift-cases-duck DUCK_PORT=$((SIFT_PORT + 1))
# fsync and autovacuum are off in the test cluster; wal_level and full_page_writes stay as they are: bulk_copy_wal measures the WAL a COPY writes
# the library is loaded when the server starts: the first statement of a backend is planned with it, and the shared table of verdicts (sift.verdict_cache_kb) exists
SETTINGS=(shared_preload_libraries=pg_sift max_locks_per_transaction=16384 shared_buffers=1GB effective_cache_size=3GB work_mem=64MB maintenance_work_mem=256MB max_wal_size=4GB jit=off max_worker_processes=16 max_parallel_workers=16
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
  pg_container "$DUCK" "$DUCK_PORT" "$STORE_DUCKDB_IMAGE" "${PG_FLAGS[@]}" -c max_parallel_workers_per_gather=$((CPUS - 1)) -c duckdb.threads="$CPUS" -c duckdb.threads_for_postgres_scan="$CPUS" -c duckdb.max_workers_per_postgres_scan=$((CPUS - 1))
  psql -X -q -h 127.0.0.1 -p "$DUCK_PORT" -U postgres -d postgres -c "DROP DATABASE IF EXISTS bench WITH (FORCE)" -c "CREATE DATABASE bench"
  PY+=(--duck-port "$DUCK_PORT" --engines "$ENGINES")
fi

if [[ -n $DISC ]]; then locked python3 -I "$STEPS/cases_discover.py" ${DISCOVER[@]+"${DISCOVER[@]}"}; exit; fi
locked python3 -I "$STEPS/cases.py" run --rows "$ROWS" --rounds "$ROUNDS" --runs "$RUNS" ${DOCS:+--docs} ${PY[@]+"${PY[@]}"}
