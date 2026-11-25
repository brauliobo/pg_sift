#!/usr/bin/env bash
# The memory slope of Format 1 relations (scripts/mem-slope.py, test/mem-slope-v1.tsv): what an upgraded 5.x sift_table holds while a whole-table UPDATE, a DELETE and a VACUUM
# run at 50,000, 100,000 and 200,000 rows with work_mem and maintenance_work_mem at 1 MB and the default sift.backend_cache_kb. A DELETE and a VACUUM are clean; the rows that are
# open say in their REASON the memory of the caches the operation fills, and their ceiling fails a slope that goes back up to what the tombstones held (they follow work_mem).
# Only the testing library creates Format 1 (sift.test_create_format1), so the release library skips.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init v1s 97 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.async_maintenance = off"
node_start v1s || bail "start failed"
if [[ $(sql v1s "SHOW debug_assertions") == on ]]; then echo '1..0 # SKIP an assert build checks every allocation: its resident peaks are not those of a release server'; exit 0; fi
sql v1s "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
if [[ $(sql v1s "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'") == 0 ]]; then echo '1..0 # SKIP the release library creates only Format 2.0'; exit 0; fi
sql v1s "CREATE EXTENSION pg_sift_test" "ALTER SYSTEM SET sift.test_create_format1 = on" "SELECT pg_reload_conf()" >/dev/null || bail "format setting failed"
wait_for "[[ \$(sql v1s 'SHOW sift.test_create_format1') == on ]]" 10 || bail "Format 1 not set"

slope() { PGHOST=$(dir v1s)/sock PGPORT=$(port v1s) PGUSER=postgres PGDATABASE=postgres MEM_SLOPE_TSV="$REPO/test/mem-slope-v1.tsv" python3 -I "$REPO/scripts/mem-slope.py" "$@"; }
out=$(slope run --rows "${MEM_SLOPE_ROWS:-50000}" 2>"$(dir v1s)/slope.err")
lines=$(grep '^SLOPE ' <<<"$out")
scopes=$(grep '^SCOPES ' <<<"$out" || true)
[[ -z $scopes ]] || diag "$scopes"
n=$(grep -c . <<<"$lines")
plan $((n + 1))
check "every case ran" test "$n" -eq "$(slope list | grep -c .)"
while IFS= read -r l; do
	[[ -n $l ]] || continue
	slope_case "$l"
done <<<"$lines"
finish
