#!/usr/bin/env bash
# The extents a lazy VACUUM releases wait for the next xid (store_update, P4). A lazy VACUUM is left out of snapshots and of the xid horizon, its own xid
# included, so a reader that began after the VACUUM took its xid has an xmin past it, and holds the directory as it was before the release. The release
# waits at sift.test_release_gate (a testing build) once the xid is taken; the reader starts there and waits for its map (sift.test_vis_pause); the VACUUM
# releases, new rows take every free extent, and the reader, let go, must find its segments as it left them. No timing is involved. The stress of the
# same invariant, with inserters, deleters and VACUUM side by side, is maint_readers.
. "$(dirname "$0")/lib.bash"

STABLE=3000
plan 4
node_init p4 20 "autovacuum = off
sift.async_maintenance = off"
node_start p4 || bail "start failed"
D=$(dir p4)
sql p4 "CREATE EXTENSION pg_sift" "CREATE TABLE t (id serial, body text)" \
	"INSERT INTO t (body) SELECT 'zzstable s' || i || ' ' || string_agg('w' || (random() * 2000)::int, ' ') FROM generate_series(1, $STABLE) i, generate_series(1, 20) GROUP BY i" \
	"INSERT INTO t (body) SELECT 'zzvolatile ' || string_agg('w' || (random() * 2000)::int, ' ') FROM generate_series(1, $STABLE) i, generate_series(1, 20) GROUP BY i" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64, build_segments = 8)" "DELETE FROM t WHERE body @@> 'zzvolatile'" >/dev/null || bail "setup failed"
check "the stable rows are found" test "$(sql p4 "SET enable_seqscan = off" "SELECT count(*) FROM t WHERE body @@> 'zzstable'" | tail -1)" == "$STABLE"

# hold KEY: this session holds advisory lock (0x51F7, KEY) until $D/release.KEY exists
hold() {
	rm -f "$D/locked.$1" "$D/release.$1"
	{ echo "SELECT pg_advisory_lock(20983, $1);"; echo "\\! touch '$D/locked.$1'"; echo "\\! while [ ! -e '$D/release.$1' ]; do sleep 0.05; done"; } | sql p4 >/dev/null &
	printf -v "HOLDER_$1" %s "$!"
	until [[ -e $D/locked.$1 ]]; do sleep 0.05; done
}
waiting() { # N: at least N sessions wait for an advisory lock
	for _ in $(seq 400); do
		[[ $(sql p4 "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge $1 ]] && return 0
		sleep 0.05
	done
	return 1
}
hold 6
hold 1
sql p4 "LOAD 'pg_sift'" "SET sift.test_release_gate = on" "VACUUM t" >"$D/vacuum.out" 2>&1 &
VP=$!
waiting 1 || { touch "$D/release.6" "$D/release.1"; bail "the release did not wait at the gate: $(cat "$D/vacuum.out")"; }
# a later xid completes: the horizon, which leaves the VACUUM's own xid out, moves past it
sql p4 "SELECT txid_current()" >/dev/null
sql p4 "LOAD 'pg_sift'" "SET sift.test_vis_pause = 1" "SET enable_seqscan = off" "SET sift.enable_scan = off" \
	"SELECT count(*) FROM t WHERE body @@> 'zzstable'" >"$D/reader.out" 2>&1 &
RP=$!
waiting 2 || { touch "$D/release.6" "$D/release.1"; bail "the reader did not wait for the map: $(cat "$D/reader.out")"; }
touch "$D/release.6"
wait "$HOLDER_6" "$VP"
check "VACUUM ran without error" test "$(grep -c -E "ERROR|FAILED" "$D/vacuum.out")" == 0

# every free extent that no snapshot holds goes to new rows
sql p4 "INSERT INTO t (body) SELECT 'zzvolatile ' || string_agg('w' || (random() * 2000)::int, ' ') FROM generate_series(1, 60000) i, generate_series(1, 20) GROUP BY i % 1000" \
	"SELECT sift.promote('t_idx')" >/dev/null
touch "$D/release.1"
wait "$HOLDER_1" "$RP"
check "the reader found the segments it began on" test "$(tail -1 "$D/reader.out")" == "$STABLE"
check "verify: intact, every row indexed" test "$(sql p4 "VACUUM t" "SELECT sift.verify('t_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
finish
