#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A writer of the pairs that began after the journal of a concurrent scoped build was made records its own changes, and the validation pass records none of the pairs it adds. The wait of the build
# for older snapshots does not wait for an idle READ COMMITTED transaction or a prepared one, so such a writer can commit after the stamp of the sidecar, with an xid above the xmin of the stamp:
# the read reads the runs of its members by reference, from the records it made. This holds the build at its gates (sift.test_phase_gate + 4 before the journal window, + 6 before the validation
# snapshot, + 10 after the validation pass has read the pairs and taken the stamp and before it writes the sidecars), leaves a writer open (committed, rolled back or prepared and committed) across each
# of them, and checks, before any maintenance has run, that the build wrote the sidecar, that the runs of the members whose pairs changed after the stamp are read by reference (and none when every
# change was before it), and that every tenant reads as the join does; then that the maintenance (VACUUM, which tells the log which rows are dead, the promotion and the merge) leaves runs the read takes.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"

plan 93
node_init jw 34 "autovacuum = off
sift.col_auto_merge = off
max_prepared_transactions = 8
$SIDE_CONF"
node_start jw || bail "start failed"
D=$(dir jw)
side_setup jw nojournal && sql jw "ALTER TABLE tenant_scope ADD COLUMN note text" >/dev/null || bail "setup failed"
side_facts jw 1 24000
sql jw "VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "vacuum failed"
CIC="CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)"
GATE=7300
TAIL=4 JOURNAL=6 SIDE=10

# sess NAME opens a session, say NAME 'SQL;' sends to it, hear NAME waits for it to answer
sess() { rm -f "$D/$1.in"; mkfifo "$D/$1.in"; psql -X -At -q -h "$D/sock" -p "$(port jw)" -U postgres -d postgres <"$D/$1.in" >"$D/$1.out" 2>&1 & eval "exec {FD_$1}>\"$D/$1.in\""; }
say() { local v=FD_$1; echo "$2" >&"${!v}"; }
endsess() { local v=FD_$1; eval "exec ${!v}>&-"; }
SEQ=0
hear() { SEQ=$((SEQ + 1)); say "$1" "\\echo sync$SEQ"; wait_for "grep -q '^sync$SEQ\$' '$D/$1.out'" 30 || bail "session $1 did not answer"; }

build_event() { sql jw "SELECT coalesce(max(wait_event_type || ':' || wait_event), 'running') FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX CONCURRENTLY events_col%' AND pid <> pg_backend_pid()"; }
blocked() { [[ $(build_event) == Lock:virtualxid || $(build_event) == Lock:transactionid ]]; }
at_blocked() { wait_for blocked 60 || bail "the build is at $(build_event), not blocked on a writer"; }
at_gate() { wait_for "[[ \$(sql jw \"SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid) WHERE a.query LIKE 'CREATE INDEX CONCURRENTLY events_col%' AND NOT l.granted AND l.locktype = 'advisory' AND l.objid = $((GATE + $1)) AND a.pid <> pg_backend_pid()\") == 1 ]]" 60 || bail "the build is not at gate $1 (it is at $(build_event))"; }
past_gate() { wait_for "[[ \$(build_event) != Lock:advisory ]]" 60 || bail "the build stays at a gate"; }
gates_up() { sess H; say H "SELECT pg_advisory_lock($((GATE + TAIL))), pg_advisory_lock($((GATE + JOURNAL))), pg_advisory_lock($((GATE + SIDE)));"; hear H; }
gate_open() { say H "SELECT pg_advisory_unlock($((GATE + $1)));"; hear H; }
side_gate() { at_gate $SIDE; gate_open $SIDE; }
build_start() { (sql jw "SET sift.test_phase_gate = $GATE" "$CIC" >"$D/build.out" 2>&1; echo $? >"$D/build.rc") & BUILDER=$!; }
build_ends() { wait_for "! kill -0 $BUILDER 2>/dev/null" 120 || bail "the build does not end: $(build_event)"; wait "$BUILDER"; [[ $(cat "$D/build.rc") == 0 ]]; }

# the changes of writer K: 3 pairs inserted, 2 deleted, 1 moved to another tenant, a HOT update of the notes, facts
work() {
	echo "INSERT INTO tenant_scope VALUES ($((3 * $1 - 2)), 13), ($((3 * $1 - 1)), 13), ($((3 * $1)), 14);"
	echo "DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN ($((30 + 2 * $1)), $((31 + 2 * $1))) GROUP BY 1);"
	echo "UPDATE tenant_scope SET tenant_id = 15 WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id = $((40 + $1)) GROUP BY 1);"
	echo "UPDATE tenant_scope SET note = 'hot' WHERE dim_a_id BETWEEN $((20 + 3 * $1)) AND $((22 + 3 * $1));"
	echo "INSERT INTO events SELECT 100000 + 100 * $1 + i, 1 + i % 3, timestamptz '2026-02-01', i % 7, i % 10 FROM generate_series(1, 50) i;"
}
committed_work() { sql jw "$(work "$1")" >/dev/null || bail "the writer $1 failed"; }
open_work() { sess "$1"; say "$1" "BEGIN;"; say "$1" "$(work "$2")"; hear "$1"; ! grep -q ERROR "$D/$1.out" || bail "the writer $2 failed: $(cat "$D/$1.out")"; }

every_tenant() {
	local t
	for t in $(seq 1 15); do
		side_same jw "$(side_read "$t")" "$(side_join "$t")" || { diag "tenant $t differs"; return 1; }
	done
}
verified() { test "$(sql jw "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0 -a -z "$(sql jw "SELECT sift.col_verify('events_col', true)" 2>&1)"; }
stale_runs() { sql jw "SELECT count(*) FROM sift._test_side_valid('events_col', 0, 1) WHERE NOT valid" | tail -n 1; }
maintain() { sql jw "VACUUM (INDEX_CLEANUP on) events" "SELECT sift.col_promote('events_col')" >/dev/null; while [[ $(sql jw "SELECT sift.col_merge('events_col')" | tail -n 1) != 0 ]]; do :; done; }
# verify NAME [stale|clean|-]: what the build left, before any maintenance (the sidecar is there, and the runs of the members whose pairs changed after its stamp are read by reference: stale, none: clean), and what
# the maintenance leaves (the runs are taken: VACUUM says which rows are dead, which the log of a concurrent build does not until then); every tenant reads as the join does at both points
verify() {
	check "$1: before any maintenance the build has written the sidecar" test "$(side_format jw events_col)" == 53
	case ${2:--} in
		stale) check "$1: the runs of the dim_b whose pairs changed after the stamp are read by reference ($(stale_runs) of key 1)" test "$(stale_runs)" -ge 1 ;;
		clean) check "$1: no run is read by reference: every pair change was before the stamp" test "$(stale_runs)" == 0 ;;
	esac
	check "$1: every tenant reads as the join does" every_tenant
	check "$1: no invalid index is left and the index verifies" verified
	maintain
	check "$1: after the maintenance the read takes the runs" test "$(side_runs jw "$(side_read 1)")" -gt 0
	check "$1: ... and every tenant reads as the join does" every_tenant
	check "$1: ... and the index verifies" verified
}
scenario() { sql jw "DROP INDEX IF EXISTS events_col" >/dev/null; diag "== $1"; gates_up; build_start; }
finish_scenario() { build_ends; check "$1: the build ends" test $? == 0; verify "$@"; endsess H; }

# a writer open before the journal window: the build waits for it; it commits, rolls back or is prepared and committed
at_tail() { scenario "$1"; at_gate $TAIL; }
at_tail tail_commit; open_work W 1; gate_open $TAIL; at_blocked; say W "COMMIT;"; hear W; at_gate $JOURNAL; committed_work 9; gate_open $JOURNAL; side_gate
finish_scenario tail_commit clean; endsess W
at_tail tail_rollback; open_work W 2; gate_open $TAIL; at_blocked; say W "ROLLBACK;"; hear W; at_gate $JOURNAL; gate_open $JOURNAL; side_gate
finish_scenario tail_rollback clean; endsess W
at_tail tail_prepared; open_work W 3; say W "PREPARE TRANSACTION 'w3';"; hear W; gate_open $TAIL; at_blocked; sql jw "COMMIT PREPARED 'w3'" >/dev/null
at_gate $JOURNAL; gate_open $JOURNAL; side_gate
finish_scenario tail_prepared clean; endsess W

# a writer that begins while the pass waits for the validation: idle across the validation snapshot and the stamp, it commits after the build
journal_idle() {
	scenario "$1"; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; open_work W "$2"; committed_work "$((6 + $2))"
	gate_open $JOURNAL; side_gate; build_ends; check "$1: the build ends beside the idle writer" test $? == 0
	say W "$3"; hear W; verify "$1" stale; endsess W; endsess H
}
journal_idle journal_idle_commit 4 "COMMIT;"
journal_idle journal_idle_rollback 5 "ROLLBACK;"
scenario journal_prepared; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; open_work W 6; say W "PREPARE TRANSACTION 'w6';"; hear W
gate_open $JOURNAL; side_gate; build_ends; check "journal_prepared: the build ends beside the prepared writer" test $? == 0
sql jw "COMMIT PREPARED 'w6'" >/dev/null; verify journal_prepared stale; endsess W; endsess H

# a writer that begins after the build and stays open while the merge step writes the sidecar under its stamp, and commits after it
scenario fill_idle; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; gate_open $JOURNAL; side_gate; build_ends; check "fill_idle: the build ends" test $? == 0
open_work W 7; committed_work 12; maintain
say W "COMMIT;"; hear W; verify fill_idle; endsess W; endsess H
scenario fill_prepared; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; gate_open $JOURNAL; side_gate; build_ends; check "fill_prepared: the build ends" test $? == 0
open_work W 8; say W "PREPARE TRANSACTION 'w8';"; hear W; maintain
sql jw "COMMIT PREPARED 'w8'" >/dev/null; verify fill_prepared; endsess W; endsess H

# a writer that changes the pairs after the build has read them and before it writes the sidecars (sift.test_phase_gate + 10): its records are after the stamp
at_side() { scenario "$1"; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; gate_open $JOURNAL; at_gate $SIDE; }
at_side side_commit; committed_work 13; gate_open $SIDE
finish_scenario side_commit stale
at_side side_idle; open_work W 14; committed_work 15; gate_open $SIDE; build_ends; check "side_idle: the build ends beside the idle writer" test $? == 0
say W "COMMIT;"; hear W; verify side_idle stale; endsess W; endsess H
at_side side_prepared; open_work W 16; say W "DELETE FROM tenant_scope WHERE dim_a_id = 30 AND tenant_id = 1;"; say W "PREPARE TRANSACTION 'w16';"; hear W; gate_open $SIDE; build_ends; check "side_prepared: the build ends beside the prepared writer" test $? == 0
sql jw "COMMIT PREPARED 'w16'" >/dev/null; verify side_prepared stale; endsess W; endsess H
at_side side_rollback; open_work W 18; gate_open $SIDE; build_ends; check "side_rollback: the build ends beside the idle writer" test $? == 0
say W "ROLLBACK;"; hear W; verify side_rollback; endsess W; endsess H
finish
