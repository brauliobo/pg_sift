#!/usr/bin/env bash
# sift.maintenance_cost_delay throttles the merges of the maintenance worker, like vacuum_cost_delay: 24 promoted segments are merged
# by the worker with no delay (the default: as fast as ever) and with 30 ms for every 16 pages (128 kB) of I/O, which
# cannot take less than the number of pages the merge reads and writes allows. The throttle is counted where it acts: a testing build logs each
# nap of a throttled writer (src/core/writer.c), and the naps follow from the pages written, not from the clock or from a sample of the worker's waits, which
# a loaded runner moves. Both leave the same, intact index, and a VACUUM (not the worker) is not throttled.
. "$(dirname "$0")/lib.bash"

plan 8
node_init delay 13 "autovacuum = off"
node_start delay || bail "start failed"
sql delay "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "setup failed"

BATCH="INSERT INTO %s (body) SELECT string_agg(md5((g * %s + j)::text), ' ') FROM generate_series(1, 600) g, generate_series(1, 6) j GROUP BY g"
DELAY_MS=30
LOG=$(dir delay)/postgres.log

naps() { grep -c 'sift test: throttle nap' "$LOG" || true; }

# load TABLE: 24 promoted segments, no merge (the worker is off for the session)
load() {
	sql delay "CREATE TABLE $1 (id serial, body text)" "CREATE INDEX ${1}_idx ON $1 USING sift (body) WITH (write_buffer_limit = 1024)" >/dev/null || bail "setup $1 failed"
	sql delay "SET sift.async_maintenance = off" "DO \$\$ BEGIN FOR i IN 1..24 LOOP
		$(printf "$BATCH" "$1" "(1000 + i * 7)"); PERFORM sift.promote('${1}_idx'); COMMIT; END LOOP; END \$\$" >/dev/null || bail "load $1 failed"
}

# merged TABLE: seals the write buffer with short statements, which starts the worker, and waits until it merged the backlog (the directory holds at most
# 12 segments) and left; sets NAPS, the naps of the throttle it took
merged() {
	local before segs live
	before=$(naps)
	for k in $(seq 8); do sql delay "$(printf "$BATCH" "$1" "$((5000 + k))")" >/dev/null || bail "insert failed"; done
	for _ in $(seq 2400); do
		segs=$(sql delay "SELECT count(*) FROM sift.segments('${1}_idx')")
		live=$(sql delay "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'")
		[[ $live == 0 && $segs -le 12 ]] && break
		sleep 0.05
	done
	NAPS=$(($(naps) - before))
}

check "no delay by default" test "$(sql delay "SELECT sift.version() IS NOT NULL" "SHOW sift.maintenance_cost_delay" | tail -1)" == 0
load fast
merged fast
FAST=$NAPS
load slow
sql delay "ALTER DATABASE postgres SET sift.maintenance_cost_delay = $DELAY_MS" >/dev/null
merged slow
SLOW=$NAPS
MB=$(sql delay "SELECT sum(bytes) / 1048576 FROM sift.segments('slow_idx')")
diag "worker merge: $FAST naps with no delay, $SLOW naps at ${DELAY_MS} ms per 128 kB ($MB MB of segments after)"

# the merges read and write the whole index at least once: 8 naps for each MB (16 pages of 8 kB are 128 kB)
check "the free worker does not nap" test "$FAST" -eq 0
check "the throttled worker naps once for every 128 kB it writes, at least" test "$SLOW" -ge "$((${MB%%.*} * 8))"
check "both merged the backlog" test "$(sql delay "SELECT count(*) <= 12 FROM sift.segments('fast_idx') UNION SELECT count(*) <= 12 FROM sift.segments('slow_idx')")" == t
check "both are intact and find every row" test "$(sql delay "SELECT sift.verify('fast_idx', true)" "SELECT sift.verify('slow_idx', true)" "SELECT sift._test_check('fast_idx') + sift._test_check('slow_idx')" | tail -1)" == 0

# only the worker is throttled: VACUUM merges the 24 segments of another index with the delay set, and takes no nap
load vac
BEFORE=$(naps)
sql delay "VACUUM vac" >/dev/null
check "VACUUM is not throttled" test "$(($(naps) - BEFORE))" -eq 0
check "VACUUM merged the backlog" test "$(sql delay "SELECT count(*) <= 12 FROM sift.segments('vac_idx')")" == t
check "out of range is an error" test "$(sql delay "SELECT sift.version() IS NOT NULL" "SET sift.maintenance_cost_delay = 101" 2>&1 | grep -c 'outside the valid range')" == 1
finish
