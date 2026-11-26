#!/usr/bin/env bash
# tap: pooled
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# DML of a sift_table never waits for maintenance or for a load (audit-5.0.1 locks, findings 3 and 4): while the rewrite that VACUUM makes is in its build (the gate
# of sift.test_phase_gate, no sleep: the new tombstone log of Format 1, the outputs of a compaction of Format 2.0), a DELETE and an UPDATE complete under a
# lock_timeout; while a COPY writes (the gate inside the group writer of Format 1, after a pack the stage of Format 2.0 took), an INSERT whose write buffer fills
# rotates it under the same lock_timeout. Afterwards both end and the rows are the heap twin's. Each step holds the gate of either format.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROWS=$(knob SIFT_LM_ROWS 20000 4000)
GATE=7000
PH_LOAD="4 17"
PH_TOMB="7 11"
node_init primary 2 "autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE src AS SELECT g AS id, g % 500 AS grp, (g % 997)::numeric(10,2) AS amount, md5(g::text) || md5((g * 7)::text) AS pad FROM generate_series(1, $ROWS) g" \
	"COPY src TO '$D/load.csv' CSV" \
	"CREATE TABLE lt (id int, grp int, amount numeric(10,2), pad text) USING sift_table" \
	"CREATE TABLE ht (LIKE lt)" \
	"CREATE TABLE lt2 (id int, grp int, amount numeric(10,2), pad text) USING sift_table" "SELECT sift.table_set_option('lt2', 'write_buffer_limit', '2048kB')" >/dev/null || bail "setup failed"
plan 8

digest() { sql primary "SELECT count(*) || ':' || md5(string_agg(id || ':' || grp || ':' || amount || ':' || pad, ',' ORDER BY id)) FROM $1" | tail -1; }
HOLDER=
hold() { # PHASES: a session that holds the gates of the phases until release
	local p
	rm -f "$D/held"
	{ for p in $1; do echo "SELECT pg_advisory_lock($((GATE + p)));"; done; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	HOLDER=$!
	wait_for "[[ -e $D/held ]]" 60 || bail "the holder of the gate did not start"
}
release() {
	sql primary "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null 2>&1
	wait "$HOLDER" 2>/dev/null
}
waiting_at() { # PHASES: a backend waits for the gate of one of them
	local p ids=
	for p in $1; do ids+=${ids:+,}$((GATE + p)); done
	[[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted AND objid IN ($ids)") -ge 1 ]]
}

for t in lt ht; do sql primary "INSERT INTO $t SELECT * FROM src" >/dev/null; done
for t in lt ht; do sql primary "DELETE FROM $t WHERE id % 2 = 0" "UPDATE $t SET amount = amount + 1 WHERE id % 5 = 0" >/dev/null; done

# 1. the rewrite of the tombstone log
hold "$PH_TOMB"
sql primary "SET sift.test_phase_gate = $GATE" "VACUUM lt" >"$D/vacuum.out" 2>&1 &
VAC=$!
check "VACUUM waits in the build of the new tombstone log" wait_for "waiting_at '$PH_TOMB'" 60
out=$(sql primary "SET lock_timeout = '3s'" "UPDATE lt SET amount = amount + 1 WHERE id = 3" "DELETE FROM lt WHERE id = 5" 2>&1)  # timeout-ok: the product waits for no lock, the timeout is the failure
check "an UPDATE and a DELETE complete while the rewrite builds" test $? -eq 0
sql primary "UPDATE ht SET amount = amount + 1 WHERE id = 3" "DELETE FROM ht WHERE id = 5" >/dev/null
release
wait "$VAC"
check "the VACUUM ends" test $? -eq 0
check "the rows are the heap twin's, the entries appended during the rewrite included" test "$(digest lt)" == "$(digest ht)"
check "the store verifies" test "$(sql primary "SELECT sift._test_check('lt') >= 0")" == t

# 2. the groups of a load run
hold "$PH_LOAD"
sql primary "SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET sift.test_load_unit_bytes = $(knob SIFT_LM_UNIT 0 128)" \
	"SET sift.test_phase_gate = $GATE" "COPY lt2 FROM '$D/load.csv' CSV" >"$D/copy.out" 2>&1 &
COPY=$!
check "the COPY waits in the writing of its groups" wait_for "waiting_at '$PH_LOAD'" 60
INSERTS=$((ROWS > 6000 ? ROWS : 6000))
BURST="DO \$\$ BEGIN FOR i IN 1..$INSERTS LOOP INSERT INTO lt2 VALUES (-i, i % 500, 1, repeat(md5(i::text), 32)); END LOOP; END \$\$"
out=$(sql primary "SET lock_timeout = '3s'" "$BURST" 2>&1)  # timeout-ok: the product waits for no lock, the timeout is the failure
check "an INSERT whose write buffer fills rotates it while the run is written" test -z "$out"
release
wait "$COPY"
check "the COPY ends with its rows and the INSERT's" test "$(sql primary "SELECT count(*) FROM lt2")" == "$((ROWS + INSERTS))"
finish
