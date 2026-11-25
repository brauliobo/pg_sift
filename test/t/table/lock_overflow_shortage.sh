#!/usr/bin/env bash
# A server that cannot make the dynamic shared memory area of the row lock chunks (sift.test_dsm_fail = sift_rowlock_area, as when /dev/shm is full) does not fail the statement that fills a stripe:
# the row locks it cannot keep in shared memory go to the lock manager of PostgreSQL, both writers commit, and their rows are exact. The setup is that of tbl_lock_overflow.sh: two writers change 2,100
# rows of two tables each, more than the stripes take, and a gate (an advisory lock a test holds) makes the second one reach a full stripe while the first still holds its rows.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

out=
plan 4
node_init ls 84 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.test_create_format1 = off
deadlock_timeout = 200ms"
node_start ls || bail "start failed"
D=$(dir ls)
sql ls "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE b (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO a SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO b SELECT g, 0 FROM generate_series(1, 6000) g" >/dev/null || bail "setup failed"

bump() { echo "UPDATE $1 SET v = v + 1 WHERE k BETWEEN $2 AND $3"; }
at_gate() { [[ $(sql ls "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; }
rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(7001);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql ls >/dev/null &
holder=$!
wait_for "[[ -e $D/locked ]]" 60 || bail "the holder did not take the gate"
SHORT="SET sift.test_dsm_fail = 'sift_rowlock_area'"
sql ls "$SHORT" "BEGIN" "$(bump a 1 2100)" "$(bump b 1 2100)" "SELECT pg_advisory_lock(7001)" "COMMIT" >"$D/s1.out" 2>&1 &
s1=$!
wait_for at_gate 60 || bail "the first writer did not reach its gate"
sql ls "$SHORT" "BEGIN" "$(bump a 3001 5100)" "$(bump b 3001 5100)" "COMMIT" >"$D/s2.out" 2>&1 &
s2=$!
second_done() { ! kill -0 "$s2" 2>/dev/null || [[ $(sql ls "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND NOT granted") -ge 1 ]]; }
wait_for second_done 60 || bail "the second writer neither finished nor waited"
touch "$D/release"
wait "$holder"
wait "$s1"; r1=$?
wait "$s2"; r2=$?
check "the first writer commits" test "$r1" -eq 0 -a -z "$(grep ERROR "$D/s1.out")"
check "the second writer commits" test "$r2" -eq 0 -a -z "$(grep ERROR "$D/s2.out")"
check "no deadlock" test "$(grep -c 'deadlock detected' "$D/postgres.log")" -eq 0
check "both writers' rows are changed, nothing else" test "$(sql ls "SELECT (SELECT sum(v) FROM a) || ' ' || (SELECT sum(v) FROM b)")" == "4200 4200"
finish
