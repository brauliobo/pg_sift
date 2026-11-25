#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A statement that spills its tombstone log (sift.backend_cache_kb 16 kB) and is stopped while it writes a run and while it merges two (the gates of
# sift.test_phase_gate, no sleep): by an ERROR (cancel), by the end of its backend and by an immediate shutdown. Nothing is left in pgsql_tmp (after the backend ends,
# and after the restart), the table is as it was and verifies, and the same statement runs after. A temp_file_limit lower than the runs stops the statement with the
# error of that setting and leaves the table as it was.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_TF_ROWS 20000 6000)
GATE=7000
PH_WRITE=8
PH_MERGE=9
node_init primary 2 "autovacuum = off"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE src AS SELECT g AS id, g % 500 AS grp, (g % 997)::numeric(10,2) AS amount, md5(g::text) AS pad FROM generate_series(1, $ROWS) g" \
	"CREATE TABLE ht (LIKE src)" "CREATE TABLE lt (LIKE src) USING sift_table" "CREATE INDEX lt_grp ON lt (grp)" >/dev/null || bail "setup failed"
for t in ht lt; do sql primary "INSERT INTO $t SELECT * FROM src" >/dev/null; done
plan 18

SMALL="SET sift.backend_cache_kb = 16"
UPD="UPDATE lt SET amount = amount + 1"
digest() { sql primary "SELECT count(*) || ':' || md5(string_agg(id || ':' || grp || ':' || amount || ':' || pad, ',' ORDER BY id)) FROM $1" | tail -1; }
tmpfiles() { sql primary "SELECT count(*) FROM pg_ls_tmpdir()" | tail -1; }
HOLDER=
hold() { # PHASE
	rm -f "$D/held"
	{ echo "SELECT pg_advisory_lock($((GATE + $1)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	HOLDER=$!
	wait_for "[[ -e $D/held ]]" 60 || bail "the holder of the gate did not start"
}
release() {
	sql primary "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null 2>&1
	wait "$HOLDER" 2>/dev/null
}
waiting_at() { [[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted AND objid = $((GATE + $1))") -ge 1 ]]; }
victim() { sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE '%UPDATE lt SET amount = amount + 1%' AND pid <> pg_backend_pid() AND state = 'active' LIMIT 1"; }
want=$(digest lt)

for ph in $PH_WRITE $PH_MERGE; do
	name=$([[ $ph == $PH_WRITE ]] && echo writing || echo merging)
	# cancel
	hold "$ph"
	sql primary "SET sift.test_phase_gate = $GATE" "$SMALL" "$UPD" >"$D/upd.out" 2>&1 &
	U=$!
	check "the UPDATE waits while $name a run" wait_for "waiting_at $ph" 60
	sql primary "SELECT pg_cancel_backend($(victim))" >/dev/null
	wait "$U"
	out=$(cat "$D/upd.out")
	check "$name: the cancel ends it with the cancel error" grep -q "canceling statement" "$D/upd.out"
	release
	check "$name: the table is as it was" test "$(digest lt)" == "$want"
	check "$name: no temp file is left once the backend ended" test "$(tmpfiles)" == 0
	# immediate shutdown
	hold "$ph"
	sql primary "SET sift.test_phase_gate = $GATE" "$SMALL" "$UPD" >/dev/null 2>&1 &
	U=$!
	waiting_at "$ph" || wait_for "waiting_at $ph" 60
	SIFT_TMP=$D leak_expect 1 'terminated by signal 9'
	crash primary
	wait "$U" 2>/dev/null
	wait "$HOLDER" 2>/dev/null
	node_start primary || bail "restart after the crash failed"
	check "$name: nothing is left in pgsql_tmp after the restart" test "$(find "$D/data/base/pgsql_tmp" -type f 2>/dev/null | wc -l)" == 0
	check "$name: after the restart the table is as it was" test "$(digest lt)" == "$want"
	check "$name: and verifies" test "$(sql primary "SELECT sift._test_check('lt') >= 0")" == t
done

out=$(sql primary "SET temp_file_limit = '100kB'" "$SMALL" "$UPD" 2>&1)
check "a temp_file_limit below the runs ends the statement with the error of that setting" grep -q "temp_file_limit" <<<"$out"
check "... and the table is as it was" test "$(digest lt)" == "$want"
sql primary "$SMALL" "$UPD" >/dev/null && sql primary "UPDATE ht SET amount = amount + 1" >/dev/null
check "the same UPDATE runs after, to the heap's rows" test "$(digest lt)" == "$(digest ht)"
check "no temp file is left" test "$(tmpfiles)" == 0
finish
