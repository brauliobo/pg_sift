#!/usr/bin/env bash
# A pooled backend is steady (scripts/mem-slope-legacy.py idle, test/mem/idle-series.tsv): one backend runs 25 kinds of statements (aggregates, lookup joins, count distinct, search, partitions,
# writes, VACUUM, merges, verify, index builds) in turn, and over its last 1000 statements the memory of its contexts (exact bytes) does not grow.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init is 34 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.async_maintenance = off"
node_start is || bail "start failed"
sql is "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# the quick series: tables of 20,000 rows and 1500 statements, the last 1000 of them judged (the caches fill in the first of them); the full series is 12000 statements on tables of 100,000 rows
STATEMENTS=$(knob SIFT_IDLE_STATEMENTS 12000 1500)
ROWS=$(knob SIFT_IDLE_ROWS 50000 10000)
CACHE_KB=$(knob SIFT_IDLE_CACHE_KB 4096 1024)
out=$(PGHOST=$(dir is)/sock PGPORT=$(port is) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/mem-slope-legacy.py" idle --rows "$ROWS" --statements "$STATEMENTS" --cache-kb "$CACHE_KB" 2>"$(dir is)/idle.err")
plan 2
check "the series ran" grep -q '^SLOPE idle_series ' <<<"$out"
line=$(grep '^SLOPE idle_series ' <<<"$out")
if [[ $line == *" PASS" ]]; then ok "${line#SLOPE }"; else not_ok "${line#SLOPE }"; fi
finish
