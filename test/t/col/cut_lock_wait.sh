#!/usr/bin/env bash
# tap: pooled
# The cut of the free end of a sift_col file asks for its AccessExclusiveLock again for 250 ms, like the tail cut of Format 2, instead of once, and then gives up instead of
# waiting for a reader: VACUUM stays short and the file is cut by the next VACUUM once the reader is gone. A shorter lock_timeout ends the wait sooner.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROWS=$(knob SIFT_CL_ROWS 200000 100000)
plan 7
# Format 1: the merge leaves the free end of the file that the cut gives back; Format 2 compacts parts
node_init cut 4 "sift.test_create_format1 = on
autovacuum = off"
node_start cut || bail "start failed"
sql cut "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
size() { sql cut "SELECT pg_relation_size('t_idx')"; }
vacuum() { sql cut "SET sift.async_maintenance = off" "SET client_min_messages = info" "$@" "VACUUM (VERBOSE) t" 2>&1; }
waited_ms() { grep -o 'index "t_idx": a lock is held on it (waited [0-9]* ms)' <<<"$1" | grep -o '[0-9]* ms'; }
# a merge writes its output above its inputs: the file is left with free blocks below and a segment at its end
loosen() {
	sql cut "DROP INDEX IF EXISTS t_idx" >/dev/null
	sql cut "SET sift.async_maintenance = off" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp)" >/dev/null || bail "create failed"
	for k in 1 2 3; do
		sql cut "SET sift.async_maintenance = off" "INSERT INTO t SELECT id + $k * 10000000, grp, amount, pad FROM t WHERE id < 10000000 AND id % 5 = $k - 1" \
			"SELECT sift.col_promote('t_idx')" >/dev/null
	done
	sql cut "SELECT sift.col_merge('t_idx', true)" >/dev/null
}
# a reader that holds the table for $1 seconds
hold() { sql cut "BEGIN" "SELECT count(*) FROM t WHERE id < 10" "SELECT pg_sleep($1)" "COMMIT" >/dev/null 2>&1 & HOLDER=$!; wait_for "running cut '%pg_sleep(%'" 30; }
live() { sql cut "SELECT bytes FROM sift.col_index_info('t_idx')"; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

loosen
loose=$(size)
check "the merge left the file loose" test "$loose" -gt "$(( $(live) * 15 / 10 ))"

hold 2
t0=$(now_ms)
out=$(vacuum)
took=$(( $(now_ms) - t0 ))
diag "reader held 2 s: VACUUM took $took ms, cut waited $(waited_ms "$out"), file $loose -> $(size)"
check "the cut asked again for 250 ms and gave up" test "$(waited_ms "$out")" == "250 ms"
check "VACUUM did not wait for the reader" test "$took" -lt 1500
check "the file stays while the reader holds it" test "$(size)" -eq "$loose"
wait "$HOLDER"
vacuum >/dev/null
check "the next VACUUM cuts it" test "$(size)" -lt "$(( loose * 7 / 10 ))"

loosen
loose=$(size)
hold 4
out=$(vacuum "SET lock_timeout = '100ms'")  # timeout-ok: the lock_timeout ends the cut's wait while the reader holds the table for 4 s
diag "reader held 4 s, lock_timeout 100 ms: cut waited $(waited_ms "$out"), file $loose -> $(size)"
check "a shorter lock_timeout ends the wait sooner" test "$(waited_ms "$out")" == "100 ms"
wait "$HOLDER"
check "intact" test -z "$(sql cut "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
