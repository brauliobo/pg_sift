#!/usr/bin/env bash
# The cut of the free end of a sift_col file asks for its AccessExclusiveLock again for a few seconds, like the truncation of a heap, instead of once: a reader that
# holds the table for a moment no longer keeps VACUUM from giving the free blocks back. lock_timeout ends the wait, and the file is cut by the next VACUUM once the reader is gone.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_CL_ROWS 200000 100000)
plan 7
node_init cut 4 "autovacuum = off"
node_start cut || bail "start failed"
sql cut "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
size() { sql cut "SELECT pg_relation_size('t_idx')"; }
vacuum() { sql cut "SET sift.async_maintenance = off" "$@" "VACUUM t" >/dev/null; }
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
hold() { sql cut "BEGIN" "SELECT count(*) FROM t WHERE id < 10" "SELECT pg_sleep($1)" "COMMIT" >/dev/null 2>&1 & HOLDER=$!; sleep 0.5; }
live() { sql cut "SELECT bytes FROM sift.col_index_info('t_idx')"; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

loosen
loose=$(size)
check "the merge left the file loose" test "$loose" -gt "$(( $(live) * 15 / 10 ))"

hold 2
t0=$(now_ms)
vacuum
waited=$(( $(now_ms) - t0 ))
wait "$HOLDER"
diag "reader held 2 s: VACUUM took $waited ms, file $loose -> $(size)"
check "VACUUM waited for the reader and cut the file" test "$(size)" -lt "$(( loose * 7 / 10 ))"
check "the wait was the reader's, not more than the bound" test "$waited" -ge 1000 -a "$waited" -lt 8000

loosen
loose=$(size)
hold 4
t0=$(now_ms)
vacuum "SET lock_timeout = '500ms'"
waited=$(( $(now_ms) - t0 ))
diag "reader held 4 s, lock_timeout 500 ms: VACUUM took $waited ms, file $loose -> $(size)"
check "lock_timeout ends the wait" test "$waited" -lt 3000
check "the file stays while the reader holds it" test "$(size)" -eq "$loose"
wait "$HOLDER"
vacuum
check "the next VACUUM cuts it" test "$(size)" -lt "$(( loose * 7 / 10 ))"
check "intact" test -z "$(sql cut "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
