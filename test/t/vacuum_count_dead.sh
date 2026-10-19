#!/usr/bin/env bash
# count(*) of one term from the segments' counts of their live postings (count_by_df, sift_eval.c): the dead-posting table a segment
# keeps is refreshed by the cleanup of a VACUUM, long after its bulk delete cleared the liveness rows and the heap marked the pages
# all-visible. A count that planned in between (the table is short) and finds it refreshed when it starts must not take its dfs: they
# count the postings of the docs the VACUUM cleared. The count waits once its plan is made and the VACUUM before its cleanup refreshes
# the table (sift.test_count_gate, sift.test_cleanup_gate: a testing build), so there is no timing involved.
. "$(dirname "$0")/lib.bash"

plan 3
node_init vd 19 "autovacuum = off
sift.async_maintenance = off"
node_start vd || bail "start failed"
D=$(dir vd)
sql vd "CREATE EXTENSION pg_sift" "CREATE TABLE c (id int, body text)" \
	"INSERT INTO c SELECT i, 'zzword w' || i || ' x' || (i % 97) FROM generate_series(1, 60000) i" \
	"CREATE INDEX c_idx ON c USING sift (body)" "VACUUM c" "DELETE FROM c WHERE id % 5 = 0" "VACUUM c" "DELETE FROM c WHERE id % 7 = 0" >/dev/null || bail "setup failed"

waiting() { sql vd "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"; }
await() { # N: until N sessions wait for a lock
	for _ in $(seq 400); do [[ $(waiting) -ge $1 ]] && return; sleep 0.05; done
	touch "$D/release1" "$D/release2"
	bail "$1 sessions did not wait for the gates"
}

rm -f "$D/locked" "$D/release1" "$D/release2"
{
	echo "SELECT pg_advisory_lock(20983, 1), pg_advisory_lock(20983, 2);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release2' ]; do sleep 0.05; done"
	echo "SELECT pg_advisory_unlock(20983, 2);"
	echo "\\! while [ ! -e '$D/release1' ]; do sleep 0.05; done"
} | sql vd >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql vd "LOAD 'pg_sift'" "SET sift.test_cleanup_gate = on" "VACUUM c" >"$D/vacuum" 2>&1 &
vp=$!
await 1
sql vd "LOAD 'pg_sift'" "SET sift.test_count_gate = on" "SET enable_seqscan = off" "SELECT count(*) FROM c WHERE body @@> 'x7'" >"$D/got" 2>&1 &
rp=$!
await 2
touch "$D/release2"
wait "$vp"
touch "$D/release1"
wait "$hp" "$rp"
expected=$(sql vd "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM c WHERE body ~ '(^| )x7( |$)'" | tail -1)
diag "the count $(tail -1 "$D/got"), the heap's $expected"
check "the VACUUM ran" test "$(grep -c -E "ERROR" "$D/vacuum")" == 0
check "a refresh of the dead-posting table under the count: the count is the heap's" test "$(tail -1 "$D/got")" == "$expected"
check "the index verifies" test "$(sql vd "SELECT sift.verify('c_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
finish
