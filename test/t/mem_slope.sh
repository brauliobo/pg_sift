#!/usr/bin/env bash
# The memory slope gate (scripts/mem-slope.py, test/mem-slope.tsv): reads, maintenance and writes at 50,000, 100,000 and 200,000 rows with work_mem and
# maintenance_work_mem at 1 MB. An operation holds the memory its setting gives, not memory that follows the table: a clean case stays within the noise of a row
# (12 bytes), an open one (a defect recorded with the slope it was measured at) may not grow faster than its ceiling, and a statement repeated in one backend
# does not make its contexts grow. One SLOPE line a case; a fix of an open case turns its row to clean in the same commit.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
node_init ms 33 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.col_auto_merge = off"
node_start ms || bail "start failed"
sql ms "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

out=$(PGHOST=$(dir ms)/sock PGPORT=$(port ms) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/mem-slope.py" run 2>"$(dir ms)/slope.err")
lines=$(grep '^SLOPE ' <<<"$out")
n=$(grep -c . <<<"$lines")
plan $((n + 1))
check "every case of test/mem-slope.tsv ran" test "$n" -ge "$(grep -vc '^#\|^$' "$REPO/test/mem-slope.tsv")"
while IFS= read -r l; do
	[[ -n $l ]] || continue
	if [[ $l == *" PASS" ]]; then ok "${l#SLOPE }"; else not_ok "${l#SLOPE }"; fi
done <<<"$lines"
finish
