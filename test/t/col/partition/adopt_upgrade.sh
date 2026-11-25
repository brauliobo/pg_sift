#!/usr/bin/env bash
# The partitions of a key-cut index of a release (5.0.0 records none) across a swap of the library: this tree reads the index as it is, its first maintenance step records the partitions
# in one directory update that writes no segment and changes no generation, with deletion records pending on the units (the answers are the heap's before and after), the index verifies, and the
# release library refuses the chained directory with the format error until the index is built again.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=v5.0.0
git -C "$REPO" rev-parse -q --verify "refs/tags/$ref" >/dev/null || { echo "1..0 # SKIP no release tag $ref"; exit 0; }
newroot=$ROOT
plan 9

build_old "$ref"; oldroot=$OLDROOT
node_init up 46 'autovacuum = off
max_parallel_workers_per_gather = 0
max_parallel_maintenance_workers = 0'
use_root up "$oldroot"
node_start up || bail "cannot start $ref"
OLDBASE=("SET sift.col_auto_merge_partition_kb = 512" "SET sift.async_maintenance = off" "SET max_parallel_maintenance_workers = 0")
BASE=("SET sift.test_col_partition_kb = 512" "SET sift.async_maintenance = off" "SET max_parallel_maintenance_workers = 0")
sql up "CREATE EXTENSION pg_sift" "CREATE TABLE h (id int PRIMARY KEY, dim int NOT NULL, day date NOT NULL, v int, w int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO h SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g" \
	"${OLDBASE[@]}" "CREATE INDEX h_c ON h USING sift_col (id, dim, day, v, w) WITH (key_cut = 'dim, day', track_changes = 'id')" "VACUUM (FREEZE, ANALYZE) h" \
	"INSERT INTO h SELECT 400000 + g, 1 + g % 12, date '2026-01-01' + g % 200, g % 100, g FROM generate_series(1, 2000) g" "SELECT sift.col_promote('h_c')" \
	"SELECT sift.col_merge('h_c', true)" "DELETE FROM h WHERE id % 40 = 1" >/dev/null || bail "the release cannot be set up"
Q="SELECT string_agg(r::text, ';') FROM (SELECT dim, count(*), sum(v), sum(w) FROM h GROUP BY 1 ORDER BY 1) r"
ans() { sql up "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "$Q" 2>/dev/null | tail -n 1; }
segs() { sql up "SELECT coalesce(string_agg(gen || ':' || units || ':' || bytes || ':' || rows, ',' ORDER BY gen), '') FROM sift.col_segments('h_c') WHERE kind = 'immutable'" 2>/dev/null | tail -n 1; }
check "the release $ref wrote the index and the reads answer as the heap, deleted rows pending" test "$(ans on)" == "$(ans off)"
segs_before=$(segs)
n=$(sql up "SELECT count(*) FROM sift.col_segments('h_c') WHERE kind = 'immutable' AND bytes >= 256 * 1024" | tail -n 1)
node_stop up

use_root up "$newroot"
node_start up || bail "cannot start this tree on $ref data"
sql up "CREATE EXTENSION IF NOT EXISTS pg_sift_test" >/dev/null 2>&1
parts() { sql up "SELECT count(*) FROM sift._test_dir_parts('h_c')" 2>/dev/null | tail -n 1; }
check "this library reads the index of $ref as it is: $n segments of the size of a partition, none recorded, the answers are the heap's" test "$([[ $n -gt 4 ]] && echo many)|$(parts)|$(ans on)" == "many|0|$(ans off)"
adopted=$(sql up "${BASE[@]}" "SELECT sift._test_col_adopt('h_c')" 2>&1 | tail -n 1)
check "the first step records them: $adopted, $(parts) partitions, one for each segment of the size of a partition" test "$adopted|$(parts)" == "t|$n"
check "no segment was written and no generation changed" test "$(segs)" == "$segs_before"
check "every partition is within its bounds, the first starts at the lowest key" test "$(sql up "SELECT count(*) FILTER (WHERE rows_from_lo AND rows_below_next) || ' ' || count(*) FILTER (WHERE lo_empty) FROM sift._test_col_part_bounds('h_c')" | tail -n 1)" == "$n 1"
check "the answers are the heap's with the deletion records still pending" test "$(ans on)" == "$(ans off)"
check "the index verifies against the heap" test "$(sql up "SELECT sift.col_verify('h_c', true) IS NOT NULL" 2>/dev/null | tail -n 1)" == t
sql up "DELETE FROM h WHERE id % 40 = 5" "${BASE[@]}" "VACUUM h" "SELECT sift.col_maintain('h_c')" >/dev/null 2>&1
check "a delete, VACUUM and the rewrite on the recorded partitions keep the answers" test "$(ans on)|$(parts)" == "$(ans off)|$(parts)"
node_stop up

use_root up "$oldroot"
node_start up || bail "cannot start $ref on data this tree wrote"
refusal=$(sql up "SET sift.enable_agg = on" "$Q" 2>&1 | head -5)
check "the $ref library refuses the chained directory with the format error" test "$(grep -c -i 'format\|newer' <<<"$refusal")" -ge 1
node_stop up
use_root up "$newroot"
node_start up || bail "cannot restart this tree"
finish
