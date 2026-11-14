#!/usr/bin/env bash
# The SQL functions that are given the oid of a sift_col index lock the table before the index, as the statements do: DROP TABLE locks the table and then its indexes, so a
# function that held the index and then asked for the table waited for a DROP TABLE that waited for the index, and PostgreSQL aborted one of them ("deadlock detected"; the DROP
# when its deadlock_timeout ran out first). sift.index_advice and sift.col_tracking are held between their open and their use of the table by sift.test_col_open_gate (a testing
# build): the function waits for advisory lock 4251, which the test holds, and the DROP TABLE is started and seen waiting before the function is let go, so no timing is involved.
# Before the fix the function held only the index while it waited, the DROP got the table and waited for the index; now the function holds the table, and the DROP waits for it.
. "$(dirname "$0")/lib.bash"

node_init lo 46 "autovacuum = off
deadlock_timeout = 200ms"
node_start lo || bail "start failed"
D=$(dir lo)
sql lo "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
[[ $(sql lo "LOAD 'pg_sift'" "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_col_open_gate'") == 1 ]] || { echo "1..0 # SKIP a release library has no sift.test_col_open_gate (SIFT_TESTING=1)"; exit 0; }

plan 10
waited() { local n; for n in $(seq 150); do [[ $(sql lo "$1") == "$2" ]] && return 0; sleep 0.1; done; return 1; }
deadlocks() { grep -c 'deadlock detected' "$D/postgres.log" || true; }

# site LABEL QUERY: the function of QUERY stops after it opened the index; a DROP TABLE of the table is started and the function is let go
site() {
	local label=$1 query=$2 fp dp hp frc drc fpid before
	sql lo "DROP TABLE IF EXISTS t" "CREATE TABLE t AS SELECT g AS id, g % 7 AS k FROM generate_series(1, 2000) g" \
		"CREATE INDEX t_idx ON t USING sift_col (id, k)" >/dev/null || bail "setup of $label failed"
	before=$(deadlocks)
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(4251);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql lo >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done

	sql lo "LOAD 'pg_sift'" "SET sift.test_col_open_gate = 4251" "$query" >"$D/f.out" 2>&1 &
	fp=$!
	waited "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted" 1 || bail "$label: the function did not stop at its gate"
	fpid=$(sql lo "SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND NOT granted")
	check "$label: the function holds the table before it stops" \
		test "$(sql lo "SELECT count(*) FROM pg_locks WHERE pid = $fpid AND locktype = 'relation' AND granted AND relation = 't'::regclass")" == 1

	sql lo "DROP TABLE t" >"$D/drop.out" 2>&1 &
	dp=$!
	waited "SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND NOT granted" 1
	check "$label: the DROP TABLE waits for the table the function holds" \
		test "$(sql lo "SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND NOT granted AND relation = 't'::regclass")" == 1

	touch "$D/release"
	wait "$fp"; frc=$?
	wait "$dp"; drc=$?
	wait "$hp"
	check "$label: the function ends" test $frc == 0
	check "$label: the DROP TABLE ends" test $drc == 0
	check "$label: no deadlock" test "$(deadlocks)" == "$before"
}

site "index_advice" "SELECT count(*) FROM sift.index_advice('t_idx')"
site "col_tracking" "SELECT * FROM sift.col_tracking('t_idx')"
finish
