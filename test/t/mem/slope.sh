#!/usr/bin/env bash
# The memory slope gate (scripts/mem-slope.py, test/mem/slope.tsv): reads, maintenance and writes at 50,000, 100,000 and 200,000 rows with work_mem and
# maintenance_work_mem at 1 MB, every measurement in a fresh backend. An operation holds the memory its setting gives, not memory that follows the table: a clean case
# stays within the noise of a row (12 bytes), an open one (a defect recorded with the slope it was measured at) may not grow faster than its ceiling, and a statement
# repeated in one backend does not make its contexts grow. A row is judged on its basis: the bytes the memory scopes of sift counted (exact, the rows that grow in them) or the resident
# peak (sampled, the rows whose memory is not in a scope; mem_slope_exact.sh proves the exact verdict). One SLOPE line a case (an exact case that fails adds a SCOPES line for each memory scope: its peak at every size and its B/row, so that an outlier is attributed to the scope that moved); a fix of an open case turns its row to clean in the same commit.
# A server built with assertions is skipped: it checks every allocation, so its resident peaks are not those of a release server.
# MEM_SLOPE_SHARD=I/N runs the cases of one shard (crc32 of the case name), MEM_SLOPE_ROWS the smallest size: the slopes do not depend on which cases run beside them.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init ms 33 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.async_maintenance = off"
node_start ms || bail "start failed"
if [[ $(sql ms "SHOW debug_assertions") == on ]]; then echo '1..0 # SKIP an assert build checks every allocation: its resident peaks sit 0.7 to 3.2 MB above a normal build on the same exact context bytes'; exit 0; fi
sql ms "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

slope() { PGHOST=$(dir ms)/sock PGPORT=$(port ms) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/mem-slope.py" "$@"; }
out=$(slope run --rows "${MEM_SLOPE_ROWS:-50000}" 2>"$(dir ms)/slope.err")
lines=$(grep '^SLOPE ' <<<"$out")
scopes=$(grep '^SCOPES ' <<<"$out" || true)
[[ -z $scopes ]] || diag "$scopes"
n=$(grep -c . <<<"$lines")
plan $((n + 1))
check "every case of the shard ran" test "$n" -eq "$(slope list | grep -c .)"
while IFS= read -r l; do
	[[ -n $l ]] || continue
	slope_case "$l"
done <<<"$lines"
finish
