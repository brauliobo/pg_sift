#!/usr/bin/env bash
# A VACUUM that vacuums the b-trees of a table with parallel workers runs the cleanup of a sift_col index in the leader, in parallel mode, where no transaction id may be assigned: the cut of
# the free tail of the index takes AccessExclusiveLock, which is logged for a standby with the id of the transaction. The cut is left to a later VACUUM, and the VACUUM succeeds.
# sift.check(index, true) under live writers and an older snapshot that still sees deleted rows compares the index with the heap under one snapshot: no false corruption.
# A VACUUM (PARALLEL 2) of a sift_table cancelled while its participants compact (the gate after a share, sift.test_phase_gate) stops them before it gives their parts back: the next
# VACUUM and the check of the table are clean.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

done_testing
node_init pv 2 "wal_level = replica
autovacuum = off
sift.async_maintenance = off
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 2
min_parallel_index_scan_size = 0"
node_start pv || bail "start failed"
sql pv "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

sql pv "CREATE TABLE t_a9 (id int PRIMARY KEY, a bigint NOT NULL, b bigint NOT NULL, pad text NOT NULL)" \
	"INSERT INTO t_a9 SELECT g, g, g * 3, repeat('x', 40) FROM generate_series(1, 40000) g" \
	"CREATE INDEX t_a9_a ON t_a9 (a)" "CREATE INDEX t_a9_b ON t_a9 (b)" \
	"CREATE INDEX t_a9_c1 ON t_a9 USING sift_col (id, a)" "CREATE INDEX t_a9_c2 ON t_a9 USING sift_col (id, b)" \
	"VACUUM (PARALLEL 0) t_a9" "DELETE FROM t_a9 WHERE id > 8000" >/dev/null || bail "load failed"
out=$(sql pv "VACUUM (PARALLEL 2, VERBOSE) t_a9" 2>&1) && ok "the VACUUM (PARALLEL 2) succeeds" || not_ok "the VACUUM (PARALLEL 2) succeeds"
check "the b-trees were vacuumed by a worker" grep -q 'launched 2 parallel vacuum workers' <<<"$out"
check "no process asked for a transaction id" test "$(grep -c 'cannot assign transaction IDs' <<<"$out")" = 0
check "the indexes verify against the heap" test "$(sql pv "SELECT count(*) FROM sift.check('t_a9_c1', true)" "SELECT count(*) FROM sift.check('t_a9_c2', true)" | paste -sd+ | bc)" = 0

D=$(dir pv)
G=4200
waiting() { sql pv "SELECT count(*) FROM pg_stat_activity WHERE query LIKE '$1%' AND wait_event_type = 'Lock' AND wait_event = 'advisory'"; }
cancel_mid_shares() {
	sql pv "SET sift.test_create_format1 = off" "CREATE TABLE tp (id int, a bigint, pad text) USING sift_table" "INSERT INTO tp SELECT g, g, repeat('z', 40) FROM generate_series(1, 400000) g" \
		"VACUUM tp" "DELETE FROM tp WHERE id % 5 <> 0" >/dev/null || bail "the sift_table load failed"
	sess_open pv gate
	sess_say gate "SELECT pg_advisory_lock($((G + 16))); \\echo @@held"
	wait_for "grep -q '@@held' '$D/gate.out'" 30 || bail "the gate was not taken"
	(sql pv "LOAD 'pg_sift'" "SET sift.test_phase_gate = $G" "VACUUM (PARALLEL 2) tp" >"$D/vac.out" 2>&1; echo $? >"$D/vac.rc") &
	check "the leader and a worker wait at the gate after their shares" wait_for "[[ \$(waiting 'VACUUM (PARALLEL 2)') == 2 ]]" 60
	sql pv "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE 'VACUUM (PARALLEL 2)%' AND backend_type = 'client backend'" >/dev/null
	sess_say gate "SELECT pg_advisory_unlock($((G + 16)));"
	sess_close gate
	wait_for "[[ -f '$D/vac.rc' ]]" 120 || bail "the cancelled VACUUM did not end"
	out=$(cat "$D/vac.out")
	check "the VACUUM is cancelled" grep -q 'canceling statement due to user request' <<<"$out"
	out=$(sql pv "LOAD 'pg_sift'" "VACUUM (PARALLEL 2) tp" 2>&1)
	check "the next VACUUM (PARALLEL 2) succeeds" test -z "$out"
	out=$(sql pv "SELECT problem FROM sift.check('tp')" 2>&1)
	check "the table is clean after the cancel" test -z "$out"
	check "its rows are those of the DELETE" test "$(sql pv "SELECT count(*) FROM tp")" = 80000
}
if [[ $(sql pv "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_phase_gate'") == 1 ]]; then
	cancel_mid_shares
else
	skip 5 "the release library has no gate"
fi

sess_open pv rr
sess_say rr "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t_a9; \\echo @@pinned"
wait_for "grep -q '@@pinned' '$D/rr.out'" 30 || bail "the snapshot was not taken"
sql pv "DELETE FROM t_a9 WHERE id <= 50" "UPDATE t_a9 SET a = a + 1 WHERE id BETWEEN 100 AND 150" >/dev/null || bail "the DML failed"
out=$(sql pv "SELECT problem FROM sift.check('t_a9_c1', true)" 2>&1)
check "the heap check is clean while an older snapshot sees the deleted rows" test -z "$out"
printf '\\set k random(200000, 2000000000)\nINSERT INTO t_a9 SELECT :k + g, g, g, $$w$$ FROM generate_series(1, 50) g ON CONFLICT DO NOTHING;\n' >"$D/ins.sql"
pgbench_bg pv 2 8 -f "$D/ins.sql"
out=$(for _ in $(seq 12); do sql pv "SELECT problem FROM sift.check('t_a9_c1', true)" 2>&1; done)
wait "$PGBENCH_PID"
check "the heap check is clean under concurrent inserts" test -z "$out"
sess_say rr "COMMIT;"
sess_close rr
finish
