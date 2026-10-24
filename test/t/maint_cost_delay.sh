#!/usr/bin/env bash
# sift.maintenance_cost_delay throttles the merges of the maintenance worker, like vacuum_cost_delay: 24 promoted segments are merged
# by the worker with no delay (the default: as fast as ever) and with 100 ms (the largest) for every 16 pages (128 kB) of I/O, which
# cannot take less than the number of pages the merge reads and writes allows. The throttle is seen where it acts, in the wait
# the worker sits in while it sleeps (sampled while it merges), not in a comparison of two wall-clock times, which a loaded
# runner decides. Both leave the same, intact index, and a VACUUM (not the worker) is not throttled.
. "$(dirname "$0")/lib.bash"

plan 7
node_init delay 13 "autovacuum = off"
node_start delay || bail "start failed"
sql delay "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "setup failed"

BATCH="INSERT INTO %s (body) SELECT string_agg(md5((g * %s + j)::text), ' ') FROM generate_series(1, 1500) g, generate_series(1, 6) j GROUP BY g"
DELAY_MS=100
SAMPLE="SELECT count(*), count(*) FILTER (WHERE wait_event_type = 'Extension') FROM pg_stat_activity WHERE backend_type = 'sift maintenance'"

now_ms() { echo $(($(date +%s%N) / 1000000)); }

# load TABLE: 24 promoted segments, no merge (the worker is off for the session)
load() {
	sql delay "CREATE TABLE $1 (id serial, body text)" "CREATE INDEX ${1}_idx ON $1 USING sift (body) WITH (write_buffer_limit = 1024)" >/dev/null || bail "setup $1 failed"
	sql delay "SET sift.async_maintenance = off" "DO \$\$ BEGIN FOR i IN 1..24 LOOP
		$(printf "$BATCH" "$1" "(1000 + i * 7)"); PERFORM sift.promote('${1}_idx'); COMMIT; END LOOP; END \$\$" >/dev/null || bail "load $1 failed"
}

# merged TABLE: seals the write buffer with short statements, which starts the worker, and waits until it merged the backlog;
# sets MERGED_MS and SLEPT, the samples that found the worker in an extension wait while the backlog was still there, which is the
# throttle (the wait that follows the last merge, while the worker looks for more work, is not counted; a free worker that finds the index
# locked by an insert waits in the same way (a sanitizer build for a quarter of the merge), so the throttle is the samples it adds, not none or a ratio)
merged() {
	local t0 segs live slept
	SLEPT=0
	t0=$(now_ms)
	for k in $(seq 8); do sql delay "$(printf "$BATCH" "$1" "$((5000 + k))")" >/dev/null || bail "insert failed"; done
	for _ in $(seq 1200); do
		segs=$(sql delay "SELECT count(*) FROM sift.segments('${1}_idx')")
		IFS='|' read -r live slept <<<"$(sql delay "$SAMPLE")"
		[[ $segs -le 12 ]] || SLEPT=$((SLEPT + slept))
		[[ $live == 0 && $segs -le 12 ]] && break
		sleep 0.1
	done
	MERGED_MS=$(($(now_ms) - t0))
}

check "no delay by default" test "$(sql delay "SELECT sift.version() IS NOT NULL" "SHOW sift.maintenance_cost_delay" | tail -1)" == 0
load fast
merged fast
FAST=$MERGED_MS FAST_SLEPT=$SLEPT
load slow
sql delay "ALTER DATABASE postgres SET sift.maintenance_cost_delay = $DELAY_MS" >/dev/null
merged slow
SLOW=$MERGED_MS SLOW_SLEPT=$SLEPT
MB=$(sql delay "SELECT sum(bytes) / 1048576 FROM sift.segments('slow_idx')")
diag "worker merge: ${FAST} ms with no delay (asleep in $FAST_SLEPT samples), ${SLOW} ms at ${DELAY_MS} ms per 128 kB (asleep in $SLOW_SLEPT samples, $MB MB of segments after)"

# the merges read and write the whole index at least once: MB * 8 groups of 16 pages, each worth DELAY_MS
check "the throttled merge takes the time its I/O is worth (at least 2 s)" test "$SLOW" -ge 2000
check "the throttled worker sleeps while it merges, in at least 10 more samples than the free one" test "$SLOW_SLEPT" -ge "$((FAST_SLEPT + 10))"
check "both merged the backlog" test "$(sql delay "SELECT count(*) <= 12 FROM sift.segments('fast_idx') UNION SELECT count(*) <= 12 FROM sift.segments('slow_idx')")" == t
check "both are intact and find every row" test "$(sql delay "SELECT sift.verify('fast_idx', true)" "SELECT sift.verify('slow_idx', true)" "SELECT sift._test_check('fast_idx') + sift._test_check('slow_idx')" | tail -1)" == 0

# only the worker is throttled: VACUUM merges at full speed, in less than a throttled pass over the index takes (2 s at the least; a
# loaded runner can take that long for a free VACUUM)
FLOOR=$((${MB%%.*} * 8 * DELAY_MS))
t0=$(now_ms)
sql delay "VACUUM slow" >/dev/null
check "VACUUM is not throttled" test "$(($(now_ms) - t0))" -lt "$((FLOOR > 2000 ? FLOOR : 2000))"
check "out of range is an error" test "$(sql delay "SELECT sift.version() IS NOT NULL" "SET sift.maintenance_cost_delay = 101" 2>&1 | grep -c 'outside the valid range')" == 1
finish
