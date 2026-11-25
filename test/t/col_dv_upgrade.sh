#!/usr/bin/env bash
# Deletion records (core/v1_col/tomb/dv.h, column format minor 52) across a swap of the library: a 5.0 library writes an index that tracks its changes and its marks; this tree reads that log unchanged
# (a log of marks keeps its minor), writes records beside the old marks (the log is stamped 52 by the first record), folds and drops both; and the 5.0 library refuses the stamped log
# with the format error until the index is built again.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=v5.0.0
git -C "$REPO" rev-parse -q --verify "refs/tags/$ref" >/dev/null || { echo "1..0 # SKIP no release tag $ref"; exit 0; }
newroot=$ROOT
plan 10

build_old "$ref"; oldroot=$OLDROOT
node_init up 44 'autovacuum = off
max_parallel_workers_per_gather = 0'
use_root up "$oldroot"
node_start up || bail "cannot start $ref"
sql up "CREATE EXTENSION pg_sift" "CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO h SELECT i, i % 10, i FROM generate_series(1, 20000) i" "CREATE INDEX h_c ON h USING sift_col (id, g, v) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) h" "DELETE FROM h WHERE id % 40 = 1" >/dev/null 2>&1 || bail "the release cannot be set up"
Q="SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM h GROUP BY g) q"
ans() { sql up "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "$Q" 2>/dev/null | tail -n 1; }
tomb_format() { sql up "SELECT sift._test_col_log_format('h_c')" 2>/dev/null | tail -n 1; }
records() { sql up "SELECT count(*) FROM sift._test_col_log_records('h_c')" 2>/dev/null | tail -n 1; }
check "the release $ref wrote the marks of its delete and the reads answer as the heap" test "$(ans on)" == "$(ans off)"
node_stop up

use_root up "$newroot"
node_start up || bail "cannot start this tree on $ref data"
sql up "CREATE EXTENSION IF NOT EXISTS pg_sift_test" >/dev/null 2>&1
check "this library reads the log of marks of $ref: the answers are the heap's and the log is not stamped 52" test "$(ans on)|$([[ $(tomb_format) -lt 52 ]] && echo older)" == "$(ans off)|older"
sql up "DELETE FROM h WHERE id % 40 = 5" >/dev/null 2>&1
check "a large delete writes records beside the old marks: the log holds both and the answers are the heap's" test "$([[ $(records) -gt 0 ]] && echo records)|$(ans on)" == "records|$(ans off)"
check "the log is stamped with column format minor 52 by the first record" test "$(tomb_format)" == 52
sql up "DELETE FROM h WHERE id % 40 = 9" "SELECT sift._test_col_fold_marks('h_c')" "VACUUM h" >/dev/null 2>&1
check "the fold and VACUUM clear the old marks and the records: none is left, the answers are the heap's" test "$(records)|$(ans on)" == "0|$(ans off)"
check "the index verifies against the heap" test "$(sql up "SELECT sift.col_verify('h_c', true) IS NOT NULL" 2>/dev/null | tail -n 1)" == t
sql up "DELETE FROM h WHERE id % 40 = 13" "DROP EXTENSION pg_sift_test" >/dev/null 2>&1
node_stop up

use_root up "$oldroot"
node_start up || bail "cannot start $ref on data this tree stamped"
refusal=$(sql up "SET sift.enable_agg = on" "$Q" 2>&1 | head -5)
check "the $ref library refuses the stamped log with the format error" test "$(grep -c -i 'format' <<<"$refusal")" -ge 1
check "... and names the newer release to use" grep -q -i 'newer pg_sift\|this build reads' <<<"$refusal"
node_stop up

use_root up "$newroot"
node_start up || bail "cannot restart this tree"
sql up "CREATE EXTENSION pg_sift_test" >/dev/null 2>&1
check "this library reads the index again: the answers are the heap's with the new records pending" test "$(ans on)" == "$(ans off)"
sql up "SELECT sift._test_col_fold_marks('h_c')" "VACUUM h" >/dev/null 2>&1
check "and folds them: no record is left" test "$(records)" == 0
finish
