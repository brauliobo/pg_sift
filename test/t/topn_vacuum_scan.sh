#!/usr/bin/env bash
# A VACUUM that runs while a top-N scan (Custom Scan (SiftAgg) with Top-N) is open, after its snapshot and its source, before it reads the
# units. The scan keeps the rows its snapshot sees and, at the end,
# fetches the n winners from the heap by TID: a row of a tuple that VACUUM removed after the snapshot, on a page it made all-visible, would
# be a winner that is not in the heap ("top-N row is not in the heap"). The scan (sift.test_agg_wait) waits for an advisory lock after it
# opens, which the test holds until the statements are done: no timing involved. Stored units, a unit that was already dirty, the mutable lane.
. "$(dirname "$0")/lib.bash"

plan 4
node_init vs 14 "autovacuum = off"
node_start vs || bail "start failed"
D=$(dir vs)
sql vs "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# race TABLE: the 10 largest ids of TABLE as its snapshot sees them go to exp_TABLE, the top-N scan opens, VACUUM runs, the scan reads; the
# ids it returns go to got_TABLE (no table when it failed)
race() {
	local t=$1 hp sp
	sql vs "SET sift.enable_agg = off" "CREATE TABLE exp_$t AS SELECT id FROM $t ORDER BY id DESC LIMIT 10" >/dev/null
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(20983, 1);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql vs >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql vs "SET sift.test_agg_wait = on" "SET enable_sort = off" "CREATE TABLE got_$t AS SELECT id FROM $t ORDER BY id DESC LIMIT 10" >/dev/null 2>"$D/err_$t" &
	sp=$!
	until [[ $(sql vs "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; do sleep 0.05; done
	sql vs "VACUUM $t" >/dev/null
	touch "$D/release"
	wait "$hp" "$sp"
}

# differ TABLE: ids only the scan returned, plus ids only its snapshot had (all of them when the scan failed)
differ() {
	sql vs "SELECT (SELECT count(*) FROM (SELECT id FROM got_$1 EXCEPT SELECT id FROM exp_$1) x) + (SELECT count(*) FROM (SELECT id FROM exp_$1 EXCEPT SELECT id FROM got_$1) x)" 2>&1
}

# stored units: the index is built last, on the rows; the largest ids are the deleted ones
sql vs "CREATE TABLE stored (id int, g int, n int)" "INSERT INTO stored SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX stored_idx ON stored USING sift_col (id, g, n)" "VACUUM stored" "DELETE FROM stored WHERE id % 5 = 0" >/dev/null || bail "setup stored failed"
check "the query is a top-N scan" test "$(sql vs "SET enable_sort = off" "EXPLAIN (COSTS OFF) SELECT id FROM stored ORDER BY id DESC LIMIT 10" | grep -c 'Top-N: id DESC')" == 1
race stored
check "stored units: the top-N returns the rows of its snapshot" test "$(differ stored)" == 0

# a unit already dirty: an earlier VACUUM removed rows, and the scan opens before the next one
sql vs "CREATE TABLE dirty (id int, g int, n int)" "INSERT INTO dirty SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX dirty_idx ON dirty USING sift_col (id, g, n)" "DELETE FROM dirty WHERE id % 5 = 0" "VACUUM dirty" \
	"DELETE FROM dirty WHERE id % 7 = 0" >/dev/null || bail "setup dirty failed"
race dirty
check "dirty units: the top-N returns the rows of its snapshot" test "$(differ dirty)" == 0

# the mutable lane
sql vs "CREATE TABLE lane (id int, g int, n int)" "CREATE INDEX lane_idx ON lane USING sift_col (id, g, n) WITH (mutable_limit = 65536)" \
	"INSERT INTO lane SELECT i, i % 7, i % 100 FROM generate_series(1, 20000) i" "VACUUM lane" "DELETE FROM lane WHERE id % 5 = 0" >/dev/null || bail "setup lane failed"
race lane
check "lane rows: the top-N returns the rows of its snapshot" test "$(differ lane)" == 0
finish
