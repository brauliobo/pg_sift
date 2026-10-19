#!/usr/bin/env bash
# merge_fanin on a live index: inserts seal a hundred small segments faster than the worker (started every 2 s) merges them, so the
# directory reaches the 112 entries where inserters merge. Whatever the fanin (2, the minimum, and 32, the maximum, whose merges
# take 64 segments), no insert fails, the directory never fills (128 entries), every row is indexed, and the index settles within the bounds of the policy: fewer than fanin segments per tier, and no more entries
# than greatest(32, 2 * fanin).
. "$(dirname "$0")/lib.bash"

plan 6
node_init fanin 12 "autovacuum = off"
node_start fanin || bail "start failed"
sql fanin "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (id serial, body text)" >/dev/null || bail "setup failed"

BATCHES=$(knob SIFT_FANIN_BATCHES 150 60)
DOCS="INSERT INTO t (body) SELECT string_agg(md5(g::text || j::text || '%s'), ' ') FROM generate_series(1, 100) g, generate_series(1, 30) j GROUP BY g"
ENTRIES="SELECT count(*) FROM sift.segments('t_idx')"

# fill FANIN: a fresh table with an index of that fanin, BATCHES sealing inserts; sets MAX (most entries seen)
fill() {
	local n
	sql fanin "TRUNCATE t" "DROP INDEX IF EXISTS t_idx" "CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64, merge_fanin = $1)" >/dev/null || bail "index failed"
	MAX=0
	for k in $(seq "$BATCHES"); do
		sql fanin "$(printf "$DOCS" "$k")" >/dev/null || bail "insert failed"
		n=$(sql fanin "$ENTRIES")
		(( n > MAX )) && MAX=$n
	done
}

# the merge takes the maintenance lock, so it waits for the worker's step in progress and then merges to the fixed point of the policy
settle() { sql fanin "SELECT sift.merge('t_idx')" >/dev/null; sql fanin "$ENTRIES"; }

fill 2
SEGS2=$(settle)
diag "fanin 2: at most $MAX entries while inserting, $SEGS2 settled"
check "fanin 2: the directory never fills up (at most 120 of 128 entries)" test "$MAX" -le 120
check "fanin 2: it settles on a few segments (a tier holds one)" test "$SEGS2" -le 16
check "fanin 2: every row is indexed, the index is intact" test "$(sql fanin "SELECT sift.verify('t_idx', true)" "SELECT sift._test_check('t_idx')" | tail -1)" == 0

fill 32
SEGS32=$(settle)
diag "fanin 32: at most $MAX entries while inserting, $SEGS32 settled"
check "fanin 32: the directory never fills up (at most 120 of 128 entries)" test "$MAX" -le 120
check "fanin 32: it settles within greatest(32, 2 * fanin) entries" test "$SEGS32" -le 64
check "fanin 32: every row is indexed, the index is intact" test "$(sql fanin "SELECT sift.verify('t_idx', true)" "SELECT sift._test_check('t_idx')" | tail -1)" == 0
finish
