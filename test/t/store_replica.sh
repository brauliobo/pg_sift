#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup): churn on the primary replays to
# an identical, verifiable index on the hot standby. A standby reader whose
# segment is dropped and recycled on the primary gets the 40001 retry error
# with hot_standby_feedback off, and succeeds with it on. With sift.standby_conflicts on (the default), the primary logs the
# reuse as a recovery conflict: the reader is waited for (max_standby_streaming_delay) or cancelled like any
# standby query, whatever the feedback.
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_CHURN_SECS 15 3)
plan 10
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 16MB)"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (v int)" "INSERT INTO t VALUES (0), (5000), (300000)" \
	"CREATE INDEX t_idx ON t USING sift_store_test (v)" \
	"CREATE TABLE r (v int)" "CREATE INDEX r_idx ON r USING sift_store_test (v)" >/dev/null || bail "setup failed"

node_clone primary standby 3 "hot_standby_feedback = off
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
S=$(dir standby)
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

read -ra FILES <<<"$(churn primary t_idx "$CHURN_BYTES")"
pgbench_bg primary 8 "$SECS" "${FILES[@]}"
wait "$PGBENCH_PID"
check "churn on the primary had no failed transaction" grep -q "number of failed transactions: 0 " "$(dir primary)/pgbench.log"
catchup primary standby
SEGS="SELECT string_agg(gen || ':' || header || ':' || size, ' ' ORDER BY gen) FROM sift._test_segments('t_idx')"
check "standby directory equals the primary's" test "$(sql standby "$SEGS")" == "$(sql primary "$SEGS")"
check "standby store invariants hold, no orphan" test "$(sql standby "SELECT sift._test_check('t_idx')")" == 0
check "every segment reads back on the standby" test "$(sql standby "SELECT bool_and(sift._test_seg_read('t_idx', gen) = payload)
	FROM sift._test_segments('t_idx')")" == t

IDX=r_idx
# Recycle under a standby reader: g is dropped, recycled and overwritten; $2 runs first on the primary
recycle_race() {
	local g out
	g=$(sql primary "SELECT sift._test_seg_write('$IDX', 20000000, $1)")
	hwm=$(sql primary "SELECT value FROM sift._test_info('$IDX') WHERE key = 'high_water'")
	catchup primary standby
	sql standby "\\set VERBOSITY verbose" "SELECT sift._test_seg_read('$IDX', $g, 0, 4000)" >"$S/reader.out" 2>&1 &
	READER=$!
	sleep 2
	sql primary "${2:-SELECT 1}" "SELECT sift._test_seg_drop('$IDX', $g)" "SELECT sift._test_recycle('$IDX')" \
		"SELECT sift._test_seg_write('$IDX', 20000000, $(($1 + 1)))" >/dev/null
	catchup primary standby
	wait "$READER"
	GREW=$(sql primary "SELECT value > $hwm FROM sift._test_info('$IDX') WHERE key = 'high_water'")
}

recycle_race 100 "SET sift.standby_conflicts = off"
check "feedback off: blocks reused in place, reader gets 40001" \
	test "$GREW|$(head -1 "$S/reader.out")" == "f|ERROR:  40001: sift segment recycled during scan, retry"
diag "$(cat "$S/reader.out")"

sql standby "ALTER SYSTEM SET hot_standby_feedback = on" "SELECT pg_reload_conf()" >/dev/null
sleep 2
recycle_race 200 "SET sift.standby_conflicts = off"
check "feedback on: blocks stay pending, the primary grows, reader succeeds" \
	test "$GREW|$(cat "$S/reader.out")" == "t|20000000"
catchup primary standby

# feedback off again, on a fresh index (no fragmented free space): with the conflict logged (the default) the standby's
# replay waits for the reader
IDX=c_idx
sql primary "CREATE TABLE c (v int)" "CREATE INDEX c_idx ON c USING sift_store_test (v)" >/dev/null
sql standby "ALTER SYSTEM SET hot_standby_feedback = off" "SELECT pg_reload_conf()" >/dev/null
sleep 2
recycle_race 300
check "conflicts on, feedback off: blocks reused in place, replay waits, reader succeeds" \
	test "$GREW|$(cat "$S/reader.out")" == "f|20000000"

# a short max_standby_streaming_delay cancels the reader like any standby query
sql standby "ALTER SYSTEM SET max_standby_streaming_delay = '1s'" "SELECT pg_reload_conf()" >/dev/null
sleep 1
recycle_race 400
check "conflicts on, delay 1s: the reader is cancelled by the recovery conflict" \
	test "$(head -1 "$S/reader.out")" == "ERROR:  40001: canceling statement due to conflict with recovery"
catchup primary standby
check "standby invariants hold after the races" \
	test "$(sql standby "SELECT sift._test_check('r_idx') + sift._test_check('c_idx') + sift._test_check('t_idx')")" == 0
finish
