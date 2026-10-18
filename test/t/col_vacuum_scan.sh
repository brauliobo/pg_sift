#!/usr/bin/env bash
# A VACUUM that runs while a sift_col scan is open (after its snapshot and directory, before it reads the units). The scan must return exactly
# the rows its snapshot sees: a tuple deleted before the snapshot and removed by that VACUUM is not one of them, however the VACUUM leaves
# the heap pages (all-visible) and the liveness of the unit. Stored units, a unit that was already dirty,
# the mutable lane, and the lane next to stored units. The scan (sift._test_col_tids, flag 4) waits for an advisory lock after it opens, which the test holds
# until the VACUUM is done: no timing involved.
. "$(dirname "$0")/lib.bash"

plan 8
node_init vs 14 "autovacuum = off"
node_start vs || bail "start failed"
D=$(dir vs)
sql vs "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

# race TABLE [STATEMENT...]: the rows of TABLE go to exp_TABLE, the scan of its index opens, the statements run (default: VACUUM), the scan
# reads; its TIDs go to got_TABLE
race() {
	local t=$1 hp sp
	shift
	sql vs "CREATE TABLE exp_$t AS SELECT ctid AS tid FROM $t" >/dev/null
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(20983, 1);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql vs >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql vs "CREATE TABLE got_$t AS SELECT tid FROM sift._test_col_tids('${t}_idx', '', 4)" >/dev/null &
	sp=$!
	until [[ $(sql vs "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; do sleep 0.05; done
	sql vs "${@:-VACUUM $t}" >/dev/null
	touch "$D/release"
	wait "$hp" "$sp"
}

# differ TABLE: rows only the scan returned, plus rows only its snapshot had
differ() {
	sql vs "SELECT (SELECT count(*) FROM (SELECT tid FROM got_$1 EXCEPT SELECT tid FROM exp_$1) x) + (SELECT count(*) FROM (SELECT tid FROM exp_$1 EXCEPT SELECT tid FROM got_$1) x)"
}

# stored units: the index is built last, on the rows
sql vs "CREATE TABLE stored (id int, g int, n int)" "INSERT INTO stored SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX stored_idx ON stored USING sift_col (id, g, n)" "VACUUM stored" "DELETE FROM stored WHERE id % 5 = 0" >/dev/null || bail "setup stored failed"
race stored
check "stored units: the scan returns the rows of its snapshot" test "$(differ stored)" == 0
check "stored units: the index verifies" test "$(sql vs "SELECT sift.col_verify('stored_idx', true)" 2>&1)" == ""

# a unit already dirty: an earlier VACUUM removed rows, and the scan opens before the next one
sql vs "CREATE TABLE dirty (id int, g int, n int)" "INSERT INTO dirty SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX dirty_idx ON dirty USING sift_col (id, g, n)" "DELETE FROM dirty WHERE id % 5 = 0" "VACUUM dirty" \
	"DELETE FROM dirty WHERE id % 7 = 0" >/dev/null || bail "setup dirty failed"
race dirty
check "dirty units: the scan returns the rows of its snapshot" test "$(differ dirty)" == 0
check "dirty units: the index verifies" test "$(sql vs "SELECT sift.col_verify('dirty_idx', true)" 2>&1)" == ""

# the mutable lane, and the lane next to stored units
sql vs "CREATE TABLE lane (id int, g int, n int)" "CREATE INDEX lane_idx ON lane USING sift_col (id, g, n) WITH (mutable_limit = 65536)" \
	"INSERT INTO lane SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" "VACUUM lane" "DELETE FROM lane WHERE id % 5 = 0" >/dev/null || bail "setup lane failed"
race lane
check "lane rows: the scan returns the rows of its snapshot" test "$(differ lane)" == 0
sql vs "CREATE TABLE mixed (id int, g int, n int)" "INSERT INTO mixed SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX mixed_idx ON mixed USING sift_col (id, g, n) WITH (mutable_limit = 65536)" \
	"INSERT INTO mixed SELECT i, i % 7, i % 100 FROM generate_series(20001, 40000) i" "VACUUM mixed" "DELETE FROM mixed WHERE id % 5 = 0" >/dev/null || bail "setup mixed failed"
race mixed
check "stored units and lane rows: the scan returns the rows of its snapshot" test "$(differ mixed)" == 0

# the rows of a snapshot survive DML, VACUUM, promotion and a merge that replace the segments under the open scan
sql vs "CREATE TABLE churn (id int, g int, n int)" "INSERT INTO churn SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX churn_idx ON churn USING sift_col (id, g, n) WITH (mutable_limit = 65536)" \
	"INSERT INTO churn SELECT i, i % 7, i % 100 FROM generate_series(20001, 30000) i" "DELETE FROM churn WHERE id % 5 = 0" "VACUUM churn" >/dev/null || bail "setup churn failed"
race churn "UPDATE churn SET n = n + 1 WHERE id % 3 = 0" "DELETE FROM churn WHERE id % 11 = 0" "INSERT INTO churn SELECT i, 1, 1 FROM generate_series(30001, 33000) i" \
	"VACUUM churn" "SELECT sift.col_promote('churn_idx')" "SELECT sift.col_merge('churn_idx', true)" "VACUUM churn"
check "DML, VACUUM, promote and merge under the scan: it returns the rows of its snapshot" test "$(differ churn)" == 0
check "the index verifies after them" test "$(sql vs "SELECT sift.col_verify('churn_idx', true)" 2>&1)" == ""
finish
