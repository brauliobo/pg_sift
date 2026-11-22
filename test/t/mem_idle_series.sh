#!/usr/bin/env bash
# A pooled backend is steady (scripts/mem-slope-legacy.py idle, test/idle-series.tsv): one backend runs 25 kinds of statements (aggregates, lookup joins, count distinct, search, partitions,
# writes, VACUUM, merges, verify, index builds) in turn, and over its last 1000 statements the memory of its contexts (exact bytes) does not grow.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init is 34 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.async_maintenance = off"
node_start is || bail "start failed"
sql is "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# the quick series: the caches reach their 1 MB cap within 1500 statements, so 3000 show the last 1000 steady
STATEMENTS=$(knob SIFT_IDLE_STATEMENTS 12000 3000)
CACHE_KB=$(knob SIFT_IDLE_CACHE_KB 4096 1024)
out=$(PGHOST=$(dir is)/sock PGPORT=$(port is) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/mem-slope-legacy.py" idle --statements "$STATEMENTS" --cache-kb "$CACHE_KB" 2>"$(dir is)/idle.err")
plan 2
check "the series ran" grep -q '^SLOPE idle_series ' <<<"$out"
line=$(grep '^SLOPE idle_series ' <<<"$out")
if [[ $line == *" PASS" ]]; then ok "${line#SLOPE }"; else not_ok "${line#SLOPE }"; fi
finish
