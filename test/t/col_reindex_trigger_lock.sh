#!/usr/bin/env bash
# Two REINDEX of two sift_col indexes of one table that both have to make their change-tracking triggers (they are disabled, or the index has none yet: the option was never resolved). Each holds
# ShareLock of the table, and a REINDEX that made the triggers asked the table for a stronger lock under it, which the ShareLock of the other one refuses: one of them ended with "deadlock
# detected". The window is held open with an advisory lock (sift.test_col_track_gate: the install waits for it before it asks for anything), so that both hold ShareLock before either goes on,
# and the round does not depend on the speed of the host. A REINDEX asks for the lock only when no other session holds one that conflicts with it, and otherwise leaves the triggers: the log is
# not complete (the reads check the heap) until the maintenance worker makes missing triggers, or ALTER INDEX makes them.
. "$(dirname "$0")/lib.bash"
plan 11

ROUNDS=$(knob SIFT_RT_ROUNDS 30 10)
GATE=7311
node_init n 63 "autovacuum = off
sift.col_auto_merge = off
deadlock_timeout = 200ms"
node_start n || bail "start failed"
D=$(dir n)
sql n "CREATE EXTENSION pg_sift" \
	"CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO h SELECT i, i % 10, i FROM generate_series(1, 5000) i" \
	"CREATE INDEX h_a ON h USING sift_col (id, g)" "CREATE INDEX h_b ON h USING sift_col (id, v)" "VACUUM (FREEZE, ANALYZE) h" >/dev/null || bail "setup failed"
# one aggregate for each index (h_a has no v, h_b no g): the planner reads it from the index that holds its columns
QA="SELECT g, count(*) FROM h GROUP BY g ORDER BY 1"
QB="SELECT count(*), sum(v) FROM h WHERE id > 100"
read_with() { sql n "SET sift.col_auto_merge = on" "SET sift.enable_agg = $1" "SET seq_page_cost = 10000" "SET max_parallel_workers_per_gather = 0" "SET sift.test_no_heapfree = off" "$QA" "$QB"; }
ans() { read_with "$([[ $1 == heap ]] && echo off || echo on)"; }
tracking() { sql n "SELECT complete AND triggers FROM sift.col_tracking('$1')"; }
ntrig() { sql n "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'"; }
at_gate() { [[ $(sql n "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") == 2 ]]; }
deadlocks() { grep -c 'deadlock detected' "$D/postgres.log" || true; }
both() { [[ $(tracking h_a)'|'$(tracking h_b)'|'$(ntrig) == "t|t|4" ]]; }

# the state a round starts from: "disabled" (the triggers of both indexes are there and disabled) or "none" (no trigger, and the option of the index unset, as an index of an earlier release has it)
reset() {
	local i
	if [[ $1 == disabled ]]; then
		sql n "ALTER TABLE h DISABLE TRIGGER ALL" >/dev/null || bail "disable failed"
	else
		for i in h_a h_b; do sql n "ALTER INDEX $i SET (track_changes = 'off')" "ALTER INDEX $i RESET (track_changes)" >/dev/null || bail "reset failed"; done
	fi
}

# one round: the gate is held, both REINDEX hold ShareLock of h and wait at it, then it opens
round() {
	local a b g=$D/gate.$2
	rm -f "$g".*
	reset "$1"
	{ echo "SELECT pg_advisory_lock($GATE);"; echo "\\! touch '$g.held'"; echo "\\! while [ ! -e '$g.open' ]; do sleep 0.05; done"; } | sql n >/dev/null 2>&1 &
	g_pid=$!
	wait_for "[[ -e $g.held ]]" 60 || bail "the gate was not taken"
	sql n "SET client_min_messages = notice" "SET sift.test_col_track_gate = $GATE" "REINDEX INDEX h_a" >"$D/a.out" 2>&1 & a=$!
	sql n "SET client_min_messages = notice" "SET sift.test_col_track_gate = $GATE" "REINDEX INDEX h_b" >"$D/b.out" 2>&1 & b=$!
	wait_for at_gate 60 || bail "both REINDEX did not reach the gate"
	ROUND_SHARE=$(sql n "SELECT count(*) FROM pg_locks WHERE relation = 'h'::regclass AND mode = 'ShareLock' AND granted")
	touch "$g.open"
	wait "$a"; ROUND_RC=$?
	wait "$b"; ROUND_RC=$((ROUND_RC + $?))
	wait "$g_pid"
}
rounds() {
	local r fails=0
	for ((r = 1; r <= ROUNDS; r++)); do
		round "$1" "$1$r"
		((ROUND_RC == 0)) || fails=$((fails + 1))
	done
	ROUND_FAILS=$fails
}

# 1. the triggers are disabled
round disabled 0
check "both REINDEX hold ShareLock of the table at the gate, the triggers of both indexes disabled" test "$ROUND_SHARE" == 2
check "... and both end without an error" test "$ROUND_RC" == 0 -a "$(deadlocks)" == 0
rounds disabled
check "$ROUNDS more rounds, the triggers disabled: no failed statement, no deadlock" test "$ROUND_FAILS|$(deadlocks)" == "0|0"
check "the triggers are as the REINDEX found them (disabled), and the logs are not complete" test "$(tracking h_a)|$(tracking h_b)|$(sql n "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%' AND tgenabled = 'D'")" == "f|f|4"
check "the reads of the indexes answer as the heap does" test "$(ans free)" == "$(ans heap)"
sql n "ALTER TABLE h ENABLE TRIGGER ALL" >/dev/null
for i in h_a h_b; do sql n "ALTER INDEX $i SET (track_changes = 'off')" "ALTER INDEX $i SET (track_changes = 'id')" >/dev/null; done
check "ALTER INDEX makes the triggers and the logs complete again" both

# 2. no trigger yet: a REINDEX alone makes them (and the baseline); two at once leave them to the maintenance worker
reset none
sql n "REINDEX INDEX h_a" >/dev/null
check "a REINDEX of an index that has no triggers makes them when no other statement holds the table" test "$(tracking h_a)|$(ntrig)" == "t|2"
rounds none
check "$ROUNDS rounds of two REINDEX that must make them: no failed statement, no deadlock" test "$ROUND_FAILS|$(deadlocks)" == "0|0"
check "... the REINDEX that found the other one left the triggers, and said so" grep -q 'does not wait\|not what the option makes' "$D/a.out" "$D/b.out"
# a read that checks the heap (rows changed since the VACUUM) asks the maintenance worker, which makes the missing triggers and then the baseline
sql n "UPDATE h SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM h WHERE id % 11 = 0" >/dev/null
asking() { read_with on >/dev/null; both; }
check "the worker makes the missing triggers and the logs are complete" wait_for asking 600
check "... and the reads answer as the heap does" test "$(ans free)" == "$(ans heap)"
finish
