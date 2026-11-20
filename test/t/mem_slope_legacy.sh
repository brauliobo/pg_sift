#!/usr/bin/env bash
# The legacy memory gate (scripts/mem-slope-legacy.py, test/mem-slope-legacy.tsv): the kinds the slope gate (mem_slope.sh) does not measure yet:
# leakx, cap, evict and pinned, in exact bytes of memory contexts at 50,000 rows with work_mem and maintenance_work_mem at 1 MB: a statement repeated in one backend
# does not make its contexts grow, and the caches hold what their budget says. One SLOPE line a case; a fix of an open case turns its row to clean in the same commit.
# The memory of a merge against the rows is col_merge_exact_memory.sh.
# The shared dictionary ring and the maintenance worker are off: a dictionary a merge finds in the ring is not memory of its backend, and whether it finds it depends on what the
# sessions before it parsed; the worker promotes and merges the churn the setup made while the measured backend starts, so what the merge finds left to do depends on who is first.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init ms 33 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.col_auto_merge = off
sift.col_dict_shared_cache_kb = 0
sift.async_maintenance = off"
node_start ms || bail "start failed"
sql ms "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

out=$(PGHOST=$(dir ms)/sock PGPORT=$(port ms) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/mem-slope-legacy.py" run 2>"$(dir ms)/slope.err")
lines=$(grep '^SLOPE ' <<<"$out")
n=$(grep -c . <<<"$lines")
plan $((n + 1))
check "every case of test/mem-slope-legacy.tsv ran" test "$n" -ge "$(grep -vc '^#\|^$' "$REPO/test/mem-slope-legacy.tsv")"
while IFS= read -r l; do
	[[ -n $l ]] || continue
	slope_case "$l"
done <<<"$lines"
finish
