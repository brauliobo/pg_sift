#!/usr/bin/env bash
# The memory slope gate does not depend on the order of its cases or on the run (scripts/mem-slope.py determinism): seven cases of different kinds (a read, a read of a table
# another session made, a write, the maintenance steps that race the background worker when it may run, the two merges) run forward, forward again and in reverse, each measurement in a fresh backend; the slope
# of the bytes the memory scopes of sift counted (sift._test_mem_highwater(), exact) differs between the passes by no more than max(2 B/row, 15%). The resident peaks are printed beside them
# but not compared: a sampler that another job keeps off the CPU for a few milliseconds misses a short peak (0.5 to 0.9 MB on hosted runs with four jobs at once, none alone).
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init mo 34 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.async_maintenance = off"
node_start mo || bail "start failed"
sql mo "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

out=$(PGHOST=$(dir mo)/sock PGPORT=$(port mo) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/mem-slope.py" determinism --rows 30000 \
	--only '^(group_mid|table_scan|update_all|promote_col|vacuum_inserted|merge|merge_col)$' 2>"$(dir mo)/order.err")
lines=$(grep '^SLOPE ' <<<"$out")
plan 8
check "the seven cases ran" test "$(grep -c . <<<"$lines")" -eq 7
while IFS= read -r l; do
	[[ -n $l ]] || continue
	slope_case "$l"
done <<<"$lines"
finish
