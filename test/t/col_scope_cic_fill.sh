#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A scope index made with CREATE INDEX CONCURRENTLY or REINDEX INDEX CONCURRENTLY has the sidecars of its entity segments when the statement ends: the validation pass writes them from the pairs under the
# stamp of the journal, so its reads take the runs before the maintenance has run (the merge step is off, and nothing here calls the maintenance). The build is held after it has read the pairs and
# before it writes the sidecars (sift.test_phase_gate + 10): the writers of the table and of the pairs relation complete, the build holds nothing stronger than ShareUpdateExclusiveLock and asks for no lock,
# the pairs they change after the stamp are read by reference and the runs of the others are taken, and every key reads as the join does. The rows that arrive after the build wait in the write buffer.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"
plan 26

node_init cf 54 "autovacuum = off
sift.col_auto_merge = off
$SIDE_CONF"
node_start cf || bail "start failed"
D=$(dir cf)
side_setup cf || bail "setup failed"
side_facts cf 1 24000
sql cf "VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "vacuum failed"
CIC="CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)"
STRONG="'ShareLock', 'ShareRowExclusiveLock', 'ExclusiveLock', 'AccessExclusiveLock'"

every_key() {
	local t
	for t in 1 2 3; do side_same cf "$(side_read $t)" "$(side_join $t)" || { diag "key $t differs"; return 1; }; done
}
at_once() { # WHAT: the build has written the sidecars, nothing else did
	check "$1: the segments have the sidecar and the read takes its runs" test "$(side_format cf events_col)" == 53 -a "$(side_runs cf "$(side_read 1)")" -gt 0
	check "$1: every key reads as the join does" every_key
	check "$1: the index verifies and no key differs from the join" side_clean cf events_col
}

# writers of the table and of the pairs, with a short lock timeout: a lock the build held would fail them
wr() { sql cf "SET lock_timeout = '20s'" "$@" >"$D/wr.out" 2>&1; }
SEQ=0
writers() {
	SEQ=$((SEQ + 1))
	wr "INSERT INTO events SELECT 50000 * $SEQ + i, 31, timestamptz '2026-02-01', i % 7, i % 10 FROM generate_series(1, 40) i" \
		"UPDATE events SET v = v + 1 WHERE dim_a_id = 31 AND id < 3000" "DELETE FROM events WHERE dim_a_id = 31 AND id BETWEEN 3000 AND 4000"
}
pair_writers() { wr "INSERT INTO tenant_scope VALUES (5, 1), (11, 1)" "DELETE FROM tenant_scope WHERE dim_a_id = 2 AND tenant_id = 1" "UPDATE tenant_scope SET tenant_id = 9 WHERE dim_a_id = 58 AND tenant_id = 1"; }

# the build held after it has read the pairs and taken the stamp (lib_side.bash)
build_pid() { sql cf "SELECT pid FROM pg_stat_activity WHERE application_name = 'build' AND backend_type = 'client backend'"; }
held_strong() { sql cf "SELECT string_agg(DISTINCT mode, ',') FROM pg_locks WHERE pid = $BPID AND locktype = 'relation' AND granted AND mode IN ($STRONG) AND relation IN ('events'::regclass, 'tenant_scope'::regclass)"; }
asking() { sql cf "SELECT count(*) FROM pg_locks WHERE pid = $BPID AND locktype = 'relation' AND NOT granted"; }
held_build() { # SQL: the statement of a build session that stops at the gate; the writers complete while it waits
	local what=$1 stmt=$2
	side_gate_hold cf
	PGAPPNAME=build sql cf "SET sift.test_phase_gate = $SIDE_GATE_BASE" "$stmt" >"$D/build.out" 2>&1 &
	BUILD=$!
	wait_for 'side_gate_reached cf' 120 || bail "$what: the build did not reach the gate ($(head -n 3 "$D/build.out"))"
	BPID=$(build_pid)
	check "$what: at the gate the build holds nothing stronger than ShareUpdateExclusiveLock and asks for no lock" test -z "$(held_strong)" -a "$(asking)" == 0
	writers
	check "$what: INSERT, UPDATE and DELETE of the table complete while the sidecars are being written" test $? == 0
	pair_writers
	check "$what: INSERT, UPDATE and DELETE of the pairs complete while the sidecars are being written" test $? == 0
	side_gate_open cf
	wait "$BUILD"
	check "$what: the build ends" test $? == 0
}
changed_runs() { sql cf "SELECT count(*) FILTER (WHERE NOT valid) || '/' || count(*) FROM sift._test_side_valid('events_col', 0, 1)" | tail -n 1; }
# the pairs that changed after the stamp are read by reference, the runs of the others are taken, and the answer is the join's; the runs are taken once VACUUM has said which rows are dead (the log of a
# concurrent build is incomplete until then: docs/OPTIONS.md track_changes)
after_writers() {
	check "$1: before any maintenance the segments have the sidecar the build wrote" test "$(side_format cf events_col)" == 53
	check "$1: every key reads as the join does" every_key
	check "$1: the index verifies and no key differs from the join" side_clean cf events_col
	sql cf "VACUUM (INDEX_CLEANUP on) events" >/dev/null
	check "$1: the runs the pair changes left alone are taken, those they lie in are read by reference ($(changed_runs) of key 1)" test "$(side_runs cf "$(side_read 1)")" -gt 0 -a "$(changed_runs | cut -d/ -f1)" -ge 1
	check "$1: the answer is the join's" every_key
}

pairs_restored() { sql cf "DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (5, 11)" "UPDATE tenant_scope SET tenant_id = 1 WHERE dim_a_id = 58 AND tenant_id = 9" "INSERT INTO tenant_scope VALUES (2, 1)" >/dev/null; }

# a build with no writers
sql cf "$CIC" >/dev/null || bail "build failed"
at_once "CREATE INDEX CONCURRENTLY"
side_facts cf 24001 26000
check "rows after the build wait in the write buffer and are read as the join does" every_key
sql cf "VACUUM (INDEX_CLEANUP on) events" >/dev/null
check "... and once VACUUM has run the read takes the runs" test "$(side_runs cf "$(side_read 1)")" -gt 0
sql cf "REINDEX INDEX CONCURRENTLY events_col" >/dev/null || bail "reindex failed"
at_once "REINDEX INDEX CONCURRENTLY"
sql cf "DROP INDEX events_col" "TRUNCATE tenant_scope" "INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 60) m WHERE m % 6 < 5" "INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 60) m WHERE m % 3 = 0" \
	"INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 60) m WHERE m IN (7, 8)" "VACUUM (FREEZE) events, tenant_scope" >/dev/null || bail "reset failed"

# writers held out of nothing: the build is at the gate, the writers change the table and the pairs, and the build goes on
held_build "CREATE INDEX CONCURRENTLY" "$CIC"
after_writers "CREATE INDEX CONCURRENTLY with writers"
pairs_restored
held_build "REINDEX INDEX CONCURRENTLY" "REINDEX INDEX CONCURRENTLY events_col"
after_writers "REINDEX INDEX CONCURRENTLY with writers"
finish
