#!/usr/bin/env bash
# The journal of the scope relation (col/scope_journal.c) is the tombstone log of its journal index: WAL-logged, so a hot standby has the records of the pairs changed on the primary
# and a crash of the primary keeps them. What must hold on every node whatever the verdict (the standby may say it cannot compare a stamp with its snapshots): the answer of a read of
# the scope sidecar is the answer of the join, and a run whose member changed since the stamp is read by reference, on the standby as on the primary. The same holds for the scope of two paths
# (lib_twohop.bash: the grants by member, the grants by group and the memberships are three journals, and the reads run the join): the fixtures, the build, the REINDEX of the scope index and of a journal
# replay to the standby, the edits of each of the three tables are the join's on both nodes, and the records of the three journals survive a crash of the primary.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_twohop.bash"
. "$(dirname "$0")/lib_side.bash"

plan 30
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
$TWOHOP_CONF
$SIDE_CONF"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE tenant_scope (m int NOT NULL, t int NOT NULL, PRIMARY KEY (m, t)) WITH (autovacuum_enabled = off)" \
	"INSERT INTO tenant_scope SELECT i, 1 + i % 6 FROM generate_series(1, 200) i" \
	"CREATE TABLE ev (id int PRIMARY KEY, m int NOT NULL, v int NOT NULL, created timestamptz NOT NULL DEFAULT '2026-01-01', pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO ev (id, m, v, created, pad) SELECT i, 1 + i % 210, i % 10, timestamptz '2026-01-01' + i * interval '1 minute', repeat('x', 80) FROM generate_series(1, 12000) i" \
	"CREATE INDEX ts_journal ON tenant_scope USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE INDEX ev_col ON ev USING sift_col (id, m, v, created) WITH (key_order = 'm, created', key_cut = 'm, created', scope = 't: m -> tenant_scope(m, t)')" \
	"REINDEX INDEX ev_col" "VACUUM (FREEZE, ANALYZE) ev" "VACUUM tenant_scope" "CHECKPOINT" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = off
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

# the rows of the scope key 3 from the scope read, and from the join, by v (the shape whose runs answer from the sidecar)
MARK="SELECT v, count(*), sum(id) FROM ev e WHERE sift.in_scope('ev_col'::regclass, 't', e.m, 3) GROUP BY v ORDER BY v"
JOIN="SELECT v, count(*), sum(id) FROM ev e WHERE e.m IN (SELECT m FROM tenant_scope WHERE t = 3) GROUP BY v ORDER BY v"
scope_rows() { sql "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "$MARK"; }
join_rows() { sql "$1" "SET sift.enable_agg = off" "$JOIN"; }
stale_runs() { sql "$1" "SELECT count(*) FROM sift._test_side_valid('ev_col', 0, 3) WHERE NOT valid" | tail -n 1; }

check "primary: the scope read is the join" test "$(scope_rows primary)" == "$(join_rows primary)"
check "standby: the scope read is the join" test "$(scope_rows standby)" == "$(join_rows standby)"
check "primary: the read takes the runs of the sidecar" test "$(side_runs primary "$MARK")" -gt 0
diag "runs on the standby: $(side_runs standby "$MARK")"

# pairs deleted and inserted on the primary: the standby replays the journal records with them
sql primary "DELETE FROM tenant_scope WHERE t = 3 AND m % 4 = 0" "INSERT INTO tenant_scope VALUES (201, 3), (202, 3)" >/dev/null || bail "the changes failed"
catchup primary standby || bail "the standby did not catch up after the changes"
check "primary: the read after the changes is the join" test "$(scope_rows primary)" == "$(join_rows primary)"
check "standby: the read after the changes is the join" test "$(scope_rows standby)" == "$(join_rows standby)"
check "primary and standby: the runs of the changed dim_b are read by reference" test "$(stale_runs primary)" -ge 1 -a "$(stale_runs standby)" -ge 1

# a concurrent build under writers: held after it has read the pairs and taken the stamp, the writers of the facts and of the pairs go on, and the standby replays the sidecars the build writes. A session of the
# standby that took its REPEATABLE READ snapshot before the build reads, under it, as the join does
CIC2="CREATE INDEX CONCURRENTLY ev_col2 ON ev USING sift_col (id, m, v, created) WITH (key_order = 'm, created', key_cut = 'm, created', scope = 't: m -> tenant_scope(m, t)')"
MARK2="SELECT v, count(*), sum(id) FROM ev e WHERE sift.in_scope('ev_col2'::regclass, 't', e.m, 2) GROUP BY v ORDER BY v"
JOIN2="SELECT v, count(*), sum(id) FROM ev e WHERE e.m IN (SELECT m FROM tenant_scope WHERE t = 2) GROUP BY v ORDER BY v"
MARK3="SELECT v, count(*), sum(id) FROM ev e WHERE sift.in_scope('ev_col2'::regclass, 't', e.m, 3) GROUP BY v ORDER BY v"
JOIN3="SELECT v, count(*), sum(id) FROM ev e WHERE e.m IN (SELECT m FROM tenant_scope WHERE t = 3) GROUP BY v ORDER BY v"
both_same() { # NODE: keys 2 and 3 of ev_col2 read as the join does
	side_same "$1" "$MARK2" "$JOIN2" && side_same "$1" "$MARK3" "$JOIN3"
}
stale2() { sql "$1" "SELECT count(*) FILTER (WHERE NOT valid) FROM sift._test_side_valid('ev_col2', 0, 3)" | tail -n 1; }
concurrent_build() { # WHAT STATEMENT
	local what=$1 stmt=$2
	side_gate_hold primary
	PGAPPNAME=build sql primary "SET sift.test_phase_gate = $SIDE_GATE_BASE" "$stmt" >"$(dir primary)/build.out" 2>&1 &
	BUILD=$!
	wait_for 'side_gate_reached primary' 120 || bail "$what: the build did not reach the gate"
	sql primary "SET lock_timeout = '20s'" "INSERT INTO ev (id, m, v, created, pad) SELECT 900000 + i + $SEQ * 1000, 1 + i % 210, i % 10, timestamptz '2026-03-01', 'w' FROM generate_series(1, 60) i" \
		"DELETE FROM ev WHERE id = 3000 + $SEQ" "DELETE FROM tenant_scope WHERE t = 3 AND m % 4 = 1" "INSERT INTO tenant_scope VALUES (203 + $SEQ, 3), (209, 3), (10 + $SEQ, 3) ON CONFLICT DO NOTHING" >/dev/null
	check "$what: the writers of the facts and of the pairs complete while the sidecars are being written" test $? == 0
	side_gate_open primary
	wait "$BUILD"
	check "$what: the build ends" test $? == 0
}
runs_after_vacuum() { # WHAT
	sql primary "VACUUM (INDEX_CLEANUP on) ev" >/dev/null
	catchup primary standby || bail "$1: the standby did not catch up after the VACUUM"
	check "$1: the key whose pairs did not change takes the runs of the sidecar on both nodes, and both read as the join does" test "$(side_runs primary "$MARK2")" -gt 0 -a "$(side_runs standby "$MARK2")" -gt 0 \
		-a "$(both_same primary; echo $?)|$(both_same standby; echo $?)" == "0|0"
	check "$1: the standby's index verifies" test -z "$(sql standby "SELECT sift.col_verify('ev_col2', true)" 2>&1)"
}
one_build() { # WHAT STATEMENT
	SEQ=$((SEQ + 1))
	concurrent_build "$1" "$2"
	catchup primary standby || bail "$1: the standby did not catch up after the build"
	check "$1: the sidecars are there on both nodes before any maintenance of the index (format 53)" test "$(side_format primary ev_col2)|$(side_format standby ev_col2)" == "53|53"
	check "$1: the runs of the dim_b whose pairs changed after the stamp are read by reference on both nodes" test "$(stale2 primary)" -ge 1 -a "$(stale2 standby)" -ge 1
	check "$1: both nodes read as the join does" both_same primary -a both_same standby
	runs_after_vacuum "$1"
}
SEQ=0
one_build "CREATE INDEX CONCURRENTLY" "$CIC2"
one_build "REINDEX INDEX CONCURRENTLY" "REINDEX INDEX CONCURRENTLY ev_col2"

# the scope of two paths: made on the primary after the standby exists, so that the standby replays the three journal indexes and the build
twohop_setup primary || bail "the fixtures of the scope of two paths failed"
catchup primary standby || bail "the standby did not catch up after the fixtures"
check "two paths, primary: the build and its REINDEX: the reads are the join" test "$(twohop_reads primary)" == same
check "two paths, standby: the reads are the join" test "$(twohop_reads standby)" == same
THREE=(grants_by_member grants_by_group memberships_by_member)
journal_records() { local j n=0; for j in "${THREE[@]}"; do n=$((n + $(sql "$1" "SELECT count(*) FROM sift._test_col_log_entries('$j') WHERE kind IN (4, 5)"))); done; echo "$n"; }
# each edit touches one journal: a direct grant (the first), a grant to a group (the second), a member that leaves a group (the third); tenant 5 has no direct grant at first
edits=("INSERT INTO grants (tenant_id, dim_a_id) VALUES (5, 150)"
	"INSERT INTO grants (tenant_id, group_id) VALUES (5, 11)" "UPDATE memberships SET deleted_at = now() WHERE dim_a_id = 12 AND deleted_at IS NULL")
for i in 0 1 2; do
	sql primary "${edits[$i]}" >/dev/null || bail "edit $i failed"
	catchup primary standby || bail "the standby did not catch up after edit $i"
	check "two paths, edit of journal $((i + 1)): the reads are the join on both nodes" test "$(twohop_reads primary)|$(twohop_reads standby)" == "same|same"
done
sql primary "REINDEX INDEX grants_by_group" >/dev/null
catchup primary standby || bail "the standby did not catch up after the REINDEX of a journal"
check "two paths: after a REINDEX of a journal the reads are the join on both nodes" test "$(twohop_reads primary)|$(twohop_reads standby)" == "same|same"
sql primary "REINDEX INDEX hop_col" >/dev/null
catchup primary standby || bail "the standby did not catch up after the REINDEX of the scope index"
check "two paths: the REINDEX of the scope index replays and the reads are the join on both" test "$(twohop_reads primary)|$(twohop_reads standby)" == "same|same"
check "two paths, standby: the three journal indexes and the scope index verify" test -z "$(sql standby "SELECT sift.col_verify('grants_by_member', true)" "SELECT sift.col_verify('grants_by_group', true)" "SELECT sift.col_verify('memberships_by_member', true)" "SELECT sift.col_verify('hop_col', true)" 2>&1)"
sql primary "UPDATE grants SET deleted_at = now() WHERE dim_a_id = 31 AND tenant_id <> 0" "UPDATE memberships SET deleted_at = now() WHERE group_id = 5 AND dim_a_id % 9 = 0" >/dev/null
JREC=$(journal_records primary)

# a crash of the primary keeps the journal: its records are in the WAL, and the read is still the join
RECS=$(sql primary "SELECT count(*) FROM sift._test_col_log_entries('ts_journal') WHERE kind IN (4, 5)")
pg_ctl -D "$(dir primary)/data" -m immediate stop >/dev/null 2>&1
node_start primary || bail "the primary did not restart after the crash"
check "primary after a crash: the journal records and the read are kept" test "$(sql primary "SELECT count(*) FROM sift._test_col_log_entries('ts_journal') WHERE kind IN (4, 5)")|$(scope_rows primary)" == "$RECS|$(join_rows primary)"
check "two paths, primary after a crash: the records of the three journals and the reads are kept" test "$(journal_records primary)|$(twohop_reads primary)" == "$JREC|same"
finish
