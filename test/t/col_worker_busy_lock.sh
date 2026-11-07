#!/usr/bin/env bash
# A maintenance worker that finds the maintenance lock with an inserter (which promotes and leaves the merges to the worker) does not end with the merges undone: it looks again until
# sift.maintenance_naptime after it started, and a seal inside that window of its request asks for no worker of its own. The lock is held by sift._test_col_hold_maint (a testing build)
# until the test lets go of advisory lock (0x51F7, 11), so no timing is involved: the worker is seen waiting for the next look, and the merges follow once the lock is free.
. "$(dirname "$0")/lib.bash"

plan 5
node_init busy 47 "autovacuum = off
sift.maintenance_naptime = 10000"
node_start busy || bail "start failed"
D=$(dir busy)
sql busy "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text)" \
	"CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (vacuum_truncate = false, write_buffer_limit = 1024)" >/dev/null || bail "setup failed"

SEGMENTS="SELECT count(*) FROM sift.col_segments('t_idx')"
WORKER="SELECT coalesce(max(CASE WHEN wait_event = 'Extension' THEN 'waits' ELSE 'runs' END), 'none') FROM pg_stat_activity WHERE backend_type = 'sift maintenance'"
WAITING="SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"

sql busy "SET sift.async_maintenance = off" "DO \$\$ BEGIN FOR i IN 1..16 LOOP
	INSERT INTO t SELECT i * 100000 + g, g % 50, g % 997, md5(g::text) || md5((g * i)::text) FROM generate_series(1, 3000) g;
	PERFORM sift.col_promote('t_idx'); COMMIT; END LOOP; END \$\$" >/dev/null || bail "load failed"
check "segments before the worker" test "$(sql busy "$SEGMENTS")" -ge 16

rm -f "$D/locked" "$D/release"
{
	echo "SELECT pg_advisory_lock(20983, 11);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
} | sql busy >/dev/null &
gate=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql busy "SELECT sift._test_col_hold_maint('t_idx', 11)" >/dev/null &
holder=$!
until [[ $(sql busy "$WAITING") -ge 1 ]]; do sleep 0.05; done

# the seal of this insert starts the worker, which finds the lock held
sql busy "ALTER INDEX t_idx SET (write_buffer_limit = 64)" \
	"INSERT INTO t SELECT 99000000 + g, g % 50, g % 997, md5(g::text) || md5((g * 3)::text) FROM generate_series(1, 3000) g" >/dev/null || bail "insert failed"
seen=
for _ in $(seq 600); do
	state=$(sql busy "$WORKER")
	[[ $state == runs ]] && seen=1
	[[ $state == waits || ( $state == none && -n $seen ) ]] && break
	sleep 0.05
done
check "the worker that found the lock held waits for its next look" test "$state" == waits
check "nothing merged while the lock was held" test "$(sql busy "$SEGMENTS")" -ge 16

touch "$D/release"
wait "$gate" "$holder"
for _ in $(seq 300); do
	[[ $(sql busy "$SEGMENTS") -le 8 ]] && break
	sleep 0.1
done
check "the worker merged the segments once the lock was free" test "$(sql busy "$SEGMENTS")" -le 8
sql busy "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'" >/dev/null
check "intact" test -z "$(sql busy "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
