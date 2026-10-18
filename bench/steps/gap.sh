#!/usr/bin/env bash
# Usage: bench/bench store gap-profile [--sizes N,N]   (bench/steps/gap.sh, called by bench/bench)
# The profile behind docs/BENCH-GAP.md: a private PostgreSQL cluster with pg_sift (scripts/pgtest.sh, production build) under a memory cap
# (systemd-run MemoryMax, no swap: a runaway query dies alone), the store `orders` table loaded from the same generator, and per question
# EXPLAIN ANALYZE, the planning time with and without the planner hook, perf stat and perf record of one backend (bench/steps/gap.py).
# Settings: S_SIZES (default 1000000,10000000), GAP_TMP (cluster directory, default /tmp/sift-gap), GAP_PORT (56100-56299, default 56250),
# GAP_MEM (memory cap, default 12G), OUT (result directory). The same server settings as the store containers.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

SIZES=${S_SIZES:-1000000,10000000}
export SIFT_TMP=${GAP_TMP:-/tmp/sift-gap} SIFT_PORT=${GAP_PORT:-56250} SIFT_TESTING=
OUT=${OUT:-$BENCH/out/gap}
PGTEST=$REPO/scripts/pgtest.sh
capped() { systemd-run --user --scope -q -p MemoryMax="${GAP_MEM:-12G}" -p MemorySwapMax=0 -p OOMPolicy=continue "$@"; }
py() { eval "$("$PGTEST" env)"; python3 -I "$STEPS/gap.py" "$@"; }
SETTINGS=(shared_buffers=2GB effective_cache_size=6GB work_mem=64MB maintenance_work_mem=512MB max_wal_size=8GB jit=off max_worker_processes=16
          max_parallel_workers=8 max_parallel_workers_per_gather=4 max_parallel_maintenance_workers=4)

start() {
  "$PGTEST" install >/dev/null
  capped "$PGTEST" start
  eval "$("$PGTEST" env)"
  local s
  for s in "${SETTINGS[@]}"; do psql -X -q -U postgres -d postgres -c "ALTER SYSTEM SET ${s%%=*} = '${s#*=}'"; done
  "$PGTEST" stop
  capped "$PGTEST" start
}

mkdir -p "$OUT"
trap '"$PGTEST" destroy' EXIT
start
for n in ${SIZES//,/ }; do
  py load --rows "$n"
  locked py profile --rows "$n" -o "$OUT/gap-$n.json"
done
