#!/usr/bin/env bash
# Damage met while other work runs, with the gates of the testing build (advisory locks held by the test, no timing): (a) a background merge of a BM25 index holds its claimed segments while
# another session meets the damage and quarantines the index: the worker ends without an error, the index stays out of service and the statements that follow are exact; (b) a scan of a
# sift_col index held right after it opened is ended by statement_timeout (57014) and by lock_timeout (55P03): nothing is recorded and the next statement is planned on the index.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 11
node_init qc 27 "autovacuum = off
sift.on_corruption = quarantine
sift.async_maintenance = on
sift.maintenance_naptime = 1000"
node_start qc || bail "start failed"
D=$(dir qc)
sql qc "CREATE EXTENSION pg_sift_test CASCADE" "ALTER DATABASE postgres SET sift.test_room_segments = 40" \
	"ALTER DATABASE postgres SET sift.test_merge_gate = on" "CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup failed"

hold() { # NAME LOCK: a session that holds the advisory lock (the statement LOCK) until the file release.NAME appears
	local name=$1
	rm -f "$D/locked.$name" "$D/release.$name"
	{
		echo "$2;"
		echo "\\! touch '$D/locked.$name'"
		echo "\\! while [ ! -e '$D/release.$name' ]; do sleep 0.05; done"
	} | sql qc >/dev/null &
	HOLD_PID=$!
	until [[ -e $D/locked.$name ]]; do sleep 0.05; done
}
DOCS="INSERT INTO t (body) SELECT string_agg(md5(g::text || j::text), ' ') FROM generate_series(1, %s) g, generate_series(1, 30) j GROUP BY g"
WAITING="SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"
heap_rows() { sql qc "SET sift.enable_scan = off" "SELECT count(*) FROM t WHERE body @@> '$WORD'"; }

WORD=$(sql qc "SELECT md5('11')")
hold merge 'SELECT pg_advisory_lock(20983, 4)'
hp=$HOLD_PID
for _ in $(seq 120); do
	sql qc "$(printf "$DOCS" 100)" >/dev/null || bail "insert failed"
	[[ $(sql qc "$WAITING") -ge 1 ]] && break
	sleep 0.25
done
check "a background merge holds its claimed segments" test "$(sql qc "$WAITING")" -ge 1
sql qc "SELECT sift._test_poke('t_idx', 0, 24, 0, 4)" >/dev/null
out=$(sql qc "SELECT count(*) FROM t WHERE body @@> '$WORD'" 2>&1)
check "another session meets the damage and the statement fails once" grep -q 'is corrupted' <<<"$out"
check "the index is out of service" test "$(sql qc "SELECT count(*) FROM sift.quarantine WHERE index = 't_idx'::regclass")" == 1
touch "$D/release.merge"
wait "$hp"
idle() { [[ $(sql qc "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
check "the worker ends" wait_for idle 120
check "the worker did not crash and no statement failed but the one that met the damage" test "$(grep -cE 'TRAP|terminated by signal|server process .* was terminated|ERROR:  (cache lookup|could not)' "$D/postgres.log")" == 0
check "the index stays out of service" test "$(sql qc "SELECT count(*) FROM sift.quarantine WHERE index = 't_idx'::regclass")" == 1
check "the statements that follow are planned on the heap and are exact" test "$(sql qc "SELECT count(*) FROM t WHERE body @@> '$WORD'")" == "$(heap_rows)"

sql qc "CREATE TABLE c (id int, v int)" "INSERT INTO c SELECT g, g % 7 FROM generate_series(1, 20000) g" \
	"CREATE INDEX c_idx ON c USING sift_col (id, v)" "ANALYZE c" >/dev/null || bail "column index failed"
uses_sift() { sql qc "EXPLAIN (COSTS OFF) SELECT count(*) FROM c WHERE v = 3" | grep -qE 'Sift|c_idx'; }
check "the column index is planned" uses_sift
hold scan 'SELECT pg_advisory_lock(7201)'
hs=$HOLD_PID
err=$(sql qc "SET sift.test_col_open_gate = 7201" "SET statement_timeout = '500ms'" "SELECT count(*) FROM c WHERE v = 3" 2>&1)  # timeout-ok: the scan waits on the gate until the timeout the test sets ends it, the error checked
check "a held scan ends by statement_timeout" grep -q "statement timeout" <<<"$err"
err=$(sql qc "SET sift.test_col_open_gate = 7201" "SET lock_timeout = '500ms'" "SELECT count(*) FROM c WHERE v = 3" 2>&1)  # timeout-ok: the scan waits on the gate until the timeout the test sets ends it, the error checked
check "a held scan ends by lock_timeout" grep -q "lock timeout" <<<"$err"
touch "$D/release.scan"
wait "$hs"
check "nothing of the column index is recorded and it is planned again" test "$(sql qc "SELECT count(*) FROM sift.quarantine WHERE index = 'c_idx'::regclass")" == 0 -a "$(uses_sift && echo yes)" == yes
finish
