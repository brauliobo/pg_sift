#!/usr/bin/env bash
# A long merge of the background worker stalls no inserter: the worker holds a merge (its segments claimed) while a
# session inserts a hundred segments' worth into an index whose directory is "full" at 40 entries. With one exclusive
# maintenance lock for promotion and merge, the inserter waited the whole merge (the 112 of 128 entries stall); now it
# promotes and merges the other segments itself. The merge is held by sift.test_merge_gate (a testing build): the worker
# waits for advisory lock (0x51F7, 4), which the test holds, so no timing is involved.
. "$(dirname "$0")/lib.bash"

plan 6
node_init lock 9 "autovacuum = off"
node_start lock || bail "start failed"
D=$(dir lock)
# per database, which the worker reads as well
sql lock "CREATE EXTENSION pg_sift_test CASCADE" "ALTER DATABASE postgres SET sift.test_room_segments = 40" \
	"ALTER DATABASE postgres SET sift.test_merge_gate = on" "CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup failed"

DOCS="INSERT INTO t (body) SELECT string_agg(md5(g::text || j::text), ' ') FROM generate_series(1, %s) g, generate_series(1, 30) j GROUP BY g"
CLAIMS="SELECT count(*) FROM pg_locks WHERE locktype = 'object' AND classid = 't_idx'::regclass::oid AND granted"
WAITING="SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"

rm -f "$D/locked" "$D/release"
{
	echo "SELECT pg_advisory_lock(20983, 4);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
} | sql lock >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done

# segments until the worker (started at most every 2 s) is at the gate of its merge
for _ in $(seq 120); do
	sql lock "$(printf "$DOCS" 100)" >/dev/null || bail "insert failed"
	[[ $(sql lock "$WAITING") -ge 1 ]] && break
	sleep 0.25
done
check "the worker is merging claimed segments" test "$(sql lock "$CLAIMS")" -ge 2
# the exclusive SQL functions keep the steps out: with a step in flight, sift.merge waits for it and gives up at the lock timeout
check "an exclusive SQL function waits for the step in flight" grep -q "lock timeout" <(sql lock "SET lock_timeout = '1s'" "SELECT sift.merge('t_idx')" 2>&1)  # timeout-ok: the lock_timeout ends the wait for the step in flight, the failure checked

t0=$SECONDS
# a stall ends in the timeout, not in a hang: the gate is open only after this
sql lock "SET statement_timeout = '30s'" "$(printf "$DOCS" 6000)" "SELECT 'wait_ms ' || sift._test_maint_wait_us() / 1000" >"$(dir lock)/insert.out" 2>&1
rc=$?
secs=$((SECONDS - t0))
diag "insert took $secs s, $(grep wait_ms "$(dir lock)/insert.out")"
check "the insert of a hundred segments succeeds while the worker holds its merge" test "$rc" == 0
# the wall time of the insert is the load of the runner (17 s of ASan lanes, wait_ms 0, in run 40201141496): what shows it did not wait for the merge is the server's own wait below
check "no wait for the maintenance of others over 5 s" test "$(sed -n 's/^wait_ms //p' "$(dir lock)/insert.out")" -lt 5000

touch "$D/release"
wait "$hp"
sql lock "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'" >/dev/null
verified=$(sql lock "VACUUM t" "SELECT sift.verify('t_idx', true)" 2>&1 && echo verified)
check "VACUUM merges down, every row is indexed" test "$(tail -1 <<<"$verified")" == verified
check "no orphan block" test "$(sql lock "SELECT sift._test_check('t_idx')")" == 0
finish
