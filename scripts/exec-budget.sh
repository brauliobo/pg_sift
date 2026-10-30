#!/usr/bin/env bash
# The instruction ceilings of the statement classes of bench/cases.tsv (test/plan/execution.tsv, millions of user instructions of one statement at 300,000 orders): the cases of
# the file run on the cluster of scripts/pgtest.sh (SIFT_TMP, SIFT_PORT) and fail when one is above its ceiling. Skipped, exit 0, without perf.
#   scripts/exec-budget.sh [--update]      --update rewrites the ceilings from this run (the count + 15% or 0.5)
set -euo pipefail
cd "$(dirname "$0")/.."
perf stat -e instructions:u -x, true 2>&1 | grep -q instructions || { echo "exec-budget: perf cannot count user instructions, skipped"; exit 0; }
eval "$(scripts/pgtest.sh env)"
psql -X -q -U postgres -d postgres -c "DROP DATABASE IF EXISTS cases WITH (FORCE)" -c "CREATE DATABASE cases"
psql -X -q -U postgres -d cases -c "CREATE EXTENSION pg_sift"
# the settings the ceilings were recorded under (bench/steps/cases.sh): the plan and a sort that spills depend on them; the cluster's own start-up settings do not change a count
for kv in work_mem=64MB maintenance_work_mem=256MB effective_cache_size=3GB jit=off parallel_setup_cost=100 parallel_tuple_cost=0.01 min_parallel_table_scan_size=1MB min_parallel_index_scan_size=64kB; do
	psql -X -q -U postgres -d postgres -c "ALTER DATABASE cases SET ${kv%%=*} = '${kv#*=}'"
done
# one backend, as the ceilings were recorded (bench cases, one CPU): with the default of two workers the join to a key set loses its scan plan to a parallel scan of the table
export CASES_WORKERS=0
f=test/plan/execution.tsv
exec python3 -I bench/steps/cases.py run --cases "$(grep -v '^#' "$f" | cut -f1 | paste -sd, -)" --rows 300000 --rounds 1 --runs 1 --minstr --io off --no-wait \
	--budget "$f" ${1:+--budget-update} --out "${SIFT_TMP:-/tmp}/exec-budget.tsv"
