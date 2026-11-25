#!/usr/bin/env bash
# tap: pooled
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A writer of the pairs that began after the journal of a concurrent scoped build was made records its own changes, and the validation pass records none of the pairs it adds. The wait of the build
# for older snapshots does not wait for an idle READ COMMITTED transaction or a prepared one, so such a writer can commit after the stamp of the build, with an xid above the xmin of the stamp:
# the read patches its pairs in from the records it made. This holds the build at its gates (sift.test_phase_gate + 4 before the journal window, + 6 before the validation snapshot, + 5 before
# the stamp), leaves a writer open (committed, rolled back or prepared and committed) across each of them, and checks that every tenant reads as the join does, from the units of the index.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 32
node_init jw 34 "autovacuum = off
max_prepared_transactions = 8"
node_start jw || bail "start failed"
D=$(dir jw)
sql jw "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, note text, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 2) j ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + i % 300, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 9000) i" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "setup failed"
SCOPE="scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)'"
CIC="CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)"
GATE=7300
TAIL=4 JOURNAL=6 FILL=5

# sess NAME opens a session, say NAME 'SQL;' sends to it, hear NAME waits for it to answer
sess() { rm -f "$D/$1.in"; mkfifo "$D/$1.in"; psql -X -At -q -h "$(node_sock jw)" -p "$(port jw)" -U postgres -d "$(node_db jw)" <"$D/$1.in" >"$D/$1.out" 2>&1 & eval "exec {FD_$1}>\"$D/$1.in\""; }
say() { local v=FD_$1; echo "$2" >&"${!v}"; }
endsess() { local v=FD_$1; eval "exec ${!v}>&-"; }
SEQ=0
hear() { SEQ=$((SEQ + 1)); say "$1" "\\echo sync$SEQ"; wait_for "grep -q '^sync$SEQ\$' '$D/$1.out'" 30 || bail "session $1 did not answer"; }

build_event() { sql jw "SELECT coalesce(max(wait_event_type || ':' || wait_event), 'running') FROM pg_stat_activity WHERE datname = current_database() AND query LIKE 'CREATE INDEX CONCURRENTLY events_col%' AND pid <> pg_backend_pid()"; }
blocked() { [[ $(build_event) == Lock:virtualxid || $(build_event) == Lock:transactionid ]]; }
at_blocked() { wait_for blocked 60 || bail "the build is at $(build_event), not blocked on a writer"; }
at_gate() { wait_for "[[ \$(sql jw \"SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid) WHERE a.datname = current_database() AND a.query LIKE 'CREATE INDEX CONCURRENTLY events_col%' AND NOT l.granted AND l.locktype = 'advisory' AND l.objid = $((GATE + $1)) AND a.pid <> pg_backend_pid()\") == 1 ]]" 60 || bail "the build is not at gate $1 (it is at $(build_event))"; }
past_gate() { wait_for "[[ \$(build_event) != Lock:advisory ]]" 60 || bail "the build stays at a gate"; }
gates_up() { sess H; say H "SELECT pg_advisory_lock($((GATE + TAIL))), pg_advisory_lock($((GATE + JOURNAL))), pg_advisory_lock($((GATE + FILL)));"; hear H; }
gate_open() { say H "SELECT pg_advisory_unlock($((GATE + $1)));"; hear H; }
build_start() { (sql jw "SET sift.test_phase_gate = $GATE" "$CIC" >"$D/build.out" 2>&1; echo $? >"$D/build.rc") & BUILDER=$!; }
build_ends() { wait_for "! kill -0 $BUILDER 2>/dev/null" 120 || bail "the build does not end: $(build_event)"; wait "$BUILDER"; [[ $(cat "$D/build.rc") == 0 ]]; }

# the changes of writer K: 3 pairs inserted, 2 deleted, 1 moved to another tenant, a HOT update of the notes, facts
work() {
	echo "INSERT INTO tenant_scope VALUES ($((3 * $1 - 2)), 13), ($((3 * $1 - 1)), 13), ($((3 * $1)), 14);"
	echo "DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN ($((100 + 2 * $1)), $((101 + 2 * $1))) GROUP BY 1);"
	echo "UPDATE tenant_scope SET tenant_id = 15 WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id = $((200 + $1)) GROUP BY 1);"
	echo "UPDATE tenant_scope SET note = 'hot' WHERE dim_a_id BETWEEN $((240 + 3 * $1)) AND $((242 + 3 * $1));"
	echo "INSERT INTO events SELECT 100000 + 100 * $1 + i, 1 + (i * 31) % 300, timestamptz '2026-02-01', i % 10 FROM generate_series(1, 50) i;"
}
committed_work() { sql jw "$(work "$1")" >/dev/null || bail "the writer $1 failed"; }
open_work() { sess "$1"; say "$1" "BEGIN;"; say "$1" "$(work "$2")"; hear "$1"; ! grep -q ERROR "$D/$1.out" || bail "the writer $2 failed: $(cat "$D/$1.out")"; }

READ() { echo "SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, $1)"; }
PLAIN() { echo "SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = $1)"; }
verdict() {
	local p
	p=$(sql jw "SET sift.agg_force = on" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $(READ 12)")
	grep -o 'Scope Fallback: .*' <<<"$p" || { grep -q 'Scope Units' <<<"$p" && echo units || echo "no scope read"; }
}
every_tenant() {
	local t
	for t in $(seq 1 15); do
		[[ $(sql jw "SET sift.agg_force = on" "$(READ "$t")") == "$(sql jw "SET sift.enable_agg = off" "$(PLAIN "$t")")" ]] || { diag "tenant $t differs"; return 1; }
	done
}
verified() { test "$(sql jw "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0 -a -z "$(sql jw "SELECT sift.col_verify('events_col', true)" 2>&1)"; }
verify() {
	check "$1: the read is from the units ($(verdict))" test "$(verdict)" == units
	check "$1: every tenant reads as the join does" every_tenant
	check "$1: no invalid index is left and the index verifies" verified
}
scenario() { sql jw "DROP INDEX IF EXISTS events_col" >/dev/null; diag "== $1"; gates_up; build_start; }
finish_scenario() { build_ends; check "$1: the build ends" test $? == 0; verify "$1"; endsess H; }

# a writer open before the journal window: the build waits for it; it commits, rolls back or is prepared and committed
at_tail() { scenario "$1"; at_gate $TAIL; }
at_tail tail_commit; open_work W 1; gate_open $TAIL; at_blocked; say W "COMMIT;"; hear W; at_gate $JOURNAL; committed_work 9; gate_open $JOURNAL; at_gate $FILL; gate_open $FILL
finish_scenario tail_commit; endsess W
at_tail tail_rollback; open_work W 2; gate_open $TAIL; at_blocked; say W "ROLLBACK;"; hear W; at_gate $JOURNAL; gate_open $JOURNAL; at_gate $FILL; gate_open $FILL
finish_scenario tail_rollback; endsess W
at_tail tail_prepared; open_work W 3; say W "PREPARE TRANSACTION 'w3';"; hear W; gate_open $TAIL; at_blocked; sql jw "COMMIT PREPARED 'w3'" >/dev/null
at_gate $JOURNAL; gate_open $JOURNAL; at_gate $FILL; gate_open $FILL
finish_scenario tail_prepared; endsess W

# a writer that begins while the pass waits for the validation: idle across the validation snapshot and the stamp, it commits after the build
journal_idle() {
	scenario "$1"; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; open_work W "$2"; committed_work "$((6 + $2))"
	gate_open $JOURNAL; at_gate $FILL; gate_open $FILL; build_ends; check "$1: the build ends beside the idle writer" test $? == 0
	say W "$3"; hear W; verify "$1"; endsess W; endsess H
}
journal_idle journal_idle_commit 4 "COMMIT;"
journal_idle journal_idle_rollback 5 "ROLLBACK;"
scenario journal_prepared; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; open_work W 6; say W "PREPARE TRANSACTION 'w6';"; hear W
gate_open $JOURNAL; at_gate $FILL; gate_open $FILL; build_ends; check "journal_prepared: the build ends beside the prepared writer" test $? == 0
sql jw "COMMIT PREPARED 'w6'" >/dev/null; verify journal_prepared; endsess W; endsess H

# a writer that begins after the journal is valid and before the stamp, and commits after the build
scenario fill_idle; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; gate_open $JOURNAL; at_gate $FILL; open_work W 7; committed_work 12; gate_open $FILL
build_ends; check "fill_idle: the build ends beside the idle writer" test $? == 0
say W "COMMIT;"; hear W; verify fill_idle; endsess W; endsess H
scenario fill_prepared; at_gate $TAIL; gate_open $TAIL; at_gate $JOURNAL; gate_open $JOURNAL; at_gate $FILL; open_work W 8; say W "PREPARE TRANSACTION 'w8';"; hear W; gate_open $FILL
build_ends; check "fill_prepared: the build ends beside the prepared writer" test $? == 0
sql jw "COMMIT PREPARED 'w8'" >/dev/null; verify fill_prepared; endsess W; endsess H
finish
