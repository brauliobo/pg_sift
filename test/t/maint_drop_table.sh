#!/usr/bin/env bash
# DROP TABLE while the maintenance worker of a BM25 index is at work does not deadlock with it. The worker used to lock the index and then, in the middle of a merge, the table;
# DROP TABLE locks the table and then the index, so each waited for the other ("deadlock detected" in the log of the server and in the DROP). The worker now locks the table first.
# The worker is held at the point it asked for the table by sift.test_worker_gate (a testing build): it waits for advisory lock (0x51F7, 7), which the test holds, and the DROP is
# started and seen waiting before the worker is let go, so no timing is involved.
. "$(dirname "$0")/lib.bash"

plan 5
node_init dt 9 "autovacuum = off
deadlock_timeout = 200ms"
node_start dt || bail "start failed"
D=$(dir dt)
# per database, which the worker reads as well
sql dt "CREATE EXTENSION pg_sift_test CASCADE" "ALTER DATABASE postgres SET sift.test_room_segments = 40" \
	"ALTER DATABASE postgres SET sift.test_worker_gate = on" "CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup failed"

DOCS="INSERT INTO t (body) SELECT string_agg(md5(g::text || j::text), ' ') FROM generate_series(1, %s) g, generate_series(1, 30) j GROUP BY g"
WAITING="SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"
BLOCKED="SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND NOT granted"

rm -f "$D/locked" "$D/release"
{
	echo "SELECT pg_advisory_lock(20983, 7);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
} | sql dt >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done

# segments until the worker (started at most every 2 s) is at its gate
for _ in $(seq 120); do
	sql dt "$(printf "$DOCS" 100)" >/dev/null || bail "insert failed"
	[[ $(sql dt "$WAITING") -ge 1 ]] && break
	sleep 0.25
done
check "the worker is at the gate of its merge" test "$(sql dt "$WAITING")" -ge 1

sql dt "DROP TABLE t" >"$D/drop.out" 2>&1 &
dp=$!
# the DROP waits for the worker: for the index it holds, or for the table it now holds first
for _ in $(seq 300); do
	[[ $(sql dt "$BLOCKED") -ge 1 ]] && break
	sleep 0.1
done
check "the DROP TABLE waits for the worker" test "$(sql dt "$BLOCKED")" -ge 1

touch "$D/release"
wait "$hp"
wait "$dp"
rc=$?
check "the DROP TABLE succeeds" test "$rc" == 0
check "no deadlock in the log" test "$(grep -c 'deadlock detected' "$D/postgres.log")" == 0
check "the table is gone" test "$(sql dt "SELECT count(*) FROM pg_class WHERE relname IN ('t', 't_idx')")" == 0
finish
