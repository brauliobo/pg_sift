#!/usr/bin/env bash
# The journal of the scope relation (col/scope_journal.c) is the tombstone log of its journal index: WAL-logged, so a hot standby has the records of the pairs changed on the primary
# and a crash of the primary keeps them. What must hold on every node whatever the verdict (the standby may say it cannot compare a stamp with its snapshots): the answer of a read of
# the scope units is the answer of the join, and a read after a pair changed is never taken from the stored segments alone: it patches the changed members in from the entity rows, or
# says why it does not read the units. The same holds for the scope of two paths (lib_twohop.bash: the grants by member, the grants by group and the memberships are three journals): the fixtures,
# the build, the REINDEX of the scope index and of a journal replay to the standby, the edits of each of the three tables are patched in or the units are not read, and the records of the three
# journals and the stamps of the scope segments survive a crash of the primary.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_twohop.bash"

plan 20
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
$TWOHOP_CONF"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE tenant_scope (m int NOT NULL, t int NOT NULL, PRIMARY KEY (m, t)) WITH (autovacuum_enabled = off)" \
	"INSERT INTO tenant_scope SELECT i, 1 + i % 6 FROM generate_series(1, 200) i" \
	"CREATE TABLE ev (id int PRIMARY KEY, m int NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO ev SELECT i, 1 + i % 210, i % 10, repeat('x', 80) FROM generate_series(1, 6000) i" \
	"CREATE INDEX ts_journal ON tenant_scope USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE INDEX ev_col ON ev USING sift_col (id, m, v) WITH (scope = 't: m -> tenant_scope(m, t)')" \
	"REINDEX INDEX ev_col" "VACUUM (FREEZE, ANALYZE) ev" "VACUUM tenant_scope" "CHECKPOINT" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = off
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

# the rows of the scope key 3 from the scope read, and from the join; why: the fallback word, or 'stored'
MARK="SELECT count(*), sum(v) FROM ev e WHERE sift.in_scope('ev_col'::regclass, 't', e.m, 3)"
JOIN="SELECT count(*), sum(v) FROM ev e WHERE e.m IN (SELECT m FROM tenant_scope WHERE t = 3)"
scope_rows() { sql "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "$MARK"; }
join_rows() { sql "$1" "SET sift.enable_agg = off" "$JOIN"; }
scope_why() {
	sql "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SET jit = off" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $MARK" | sed -n 's/^ *Scope Fallback: //p;s/^ *Scope Units: .*/stored/p' | head -1
}

check "primary: the scope read is the join" test "$(scope_rows primary)" == "$(join_rows primary)"
check "standby: the scope read is the join" test "$(scope_rows standby)" == "$(join_rows standby)"
diag "verdict on the primary: $(scope_why primary), on the standby: $(scope_why standby)"

# pairs deleted and inserted on the primary: the standby replays the journal records with them
sql primary "DELETE FROM tenant_scope WHERE t = 3 AND m % 4 = 0" "INSERT INTO tenant_scope VALUES (201, 3), (202, 3)" >/dev/null || bail "the changes failed"
catchup primary standby || bail "the standby did not catch up after the changes"
check "primary: the read after the changes is the join" test "$(scope_rows primary)" == "$(join_rows primary)"
check "standby: the read after the changes is the join" test "$(scope_rows standby)" == "$(join_rows standby)"
scope_patched() { # the dim_b the read patched in, 0 when it patched none or did not read the units
	local n
	n=$(sql "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SET jit = off" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $MARK" | sed -n 's/^ *Scope Patched Keys: //p' | head -1)
	echo "${n:-0}"
}
check "primary: a changed member is patched in or the units are not read" test "$(scope_why primary)" != stored -o "$(scope_patched primary)" -gt 0
check "standby: a changed member is patched in or the units are not read" test "$(scope_why standby)" != stored -o "$(scope_patched standby)" -gt 0

# the scope of two paths: made on the primary after the standby exists, so that the standby replays the three journal indexes and the build
twohop_setup primary || bail "the fixtures of the scope of two paths failed"
catchup primary standby || bail "the standby did not catch up after the fixtures"
check "two paths, primary: the build and its REINDEX read the units, the three stamps vouch" test "$(twohop_reads primary)|$(twohop_why primary)|$(twohop_vouch primary)" == "same|stored|ok"
check "two paths, standby: the reads are the join" test "$(twohop_reads standby)" == same
diag "verdict on the standby: $(twohop_why standby), stamps: $(twohop_vouch standby)"
THREE=(grants_by_member grants_by_group memberships_by_member)
journal_records() { local j n=0; for j in "${THREE[@]}"; do n=$((n + $(sql "$1" "SELECT count(*) FROM sift._test_col_log_entries('$j') WHERE kind IN (4, 5)"))); done; echo "$n"; }
# each edit touches one journal: a direct grant (the first), a grant to a group (the second), a member that leaves a group (the third); tenant 5 has no direct grant at first
edits=("INSERT INTO grants (tenant_id, dim_a_id) VALUES (5, 150)"
	"INSERT INTO grants (tenant_id, group_id) VALUES (5, 11)" "UPDATE memberships SET deleted_at = now() WHERE dim_a_id = 12 AND deleted_at IS NULL")
changed_or_unread() { [[ $(twohop_why "$1") != stored || $(twohop_stat "$1" "Changed Keys") -gt 0 ]]; }
both_changed_or_unread() { changed_or_unread primary && changed_or_unread standby; }
for i in 0 1 2; do
	sql primary "${edits[$i]}" >/dev/null || bail "edit $i failed"
	catchup primary standby || bail "the standby did not catch up after edit $i"
	check "two paths, edit of journal $((i + 1)): the reads are the join on both nodes" test "$(twohop_reads primary)|$(twohop_reads standby)" == "same|same"
	diag "edit $((i + 1)): primary $(twohop_why primary) changed $(twohop_stat primary "Changed Keys"), standby $(twohop_why standby) changed $(twohop_stat standby "Changed Keys")"
	check "two paths, edit of journal $((i + 1)): the journals name the changed members or the units are not read, on both nodes" both_changed_or_unread
done
sql primary "REINDEX INDEX grants_by_group" >/dev/null
catchup primary standby || bail "the standby did not catch up after the REINDEX of a journal"
check "two paths: a REINDEX of a journal leaves the scope unvouched until its own, on both nodes the reads are the join" test "$(twohop_vouch primary)" != ok -a "$(twohop_reads primary)|$(twohop_reads standby)" == "same|same"
sql primary "REINDEX INDEX hop_col" >/dev/null
catchup primary standby || bail "the standby did not catch up after the REINDEX of the scope index"
check "two paths: the REINDEX of the scope index replays, the stamps vouch on the primary and the reads are the join on both" test "$(twohop_vouch primary)|$(twohop_reads primary)|$(twohop_reads standby)" == "ok|same|same"
diag "after the REINDEX the standby says: $(twohop_why standby), stamps: $(twohop_vouch standby)"
check "two paths, standby: the entries of the scope units are the join" test "$(sql standby "$TWOHOP_ENTRIES")" == t
check "two paths, standby: the three journal indexes and the scope index verify" test -z "$(sql standby "SELECT sift.col_verify('grants_by_member', true)" "SELECT sift.col_verify('grants_by_group', true)" "SELECT sift.col_verify('memberships_by_member', true)" "SELECT sift.col_verify('hop_col', true)" 2>&1)"
sql primary "UPDATE grants SET deleted_at = now() WHERE dim_a_id = 31 AND tenant_id <> 0" "UPDATE memberships SET deleted_at = now() WHERE group_id = 5 AND dim_a_id % 9 = 0" >/dev/null
JREC=$(journal_records primary)

# a crash of the primary keeps the journal: its records are in the WAL, and the read is still the join
RECS=$(sql primary "SELECT count(*) FROM sift._test_col_log_entries('ts_journal') WHERE kind IN (4, 5)")
pg_ctl -D "$(dir primary)/data" -m immediate stop >/dev/null 2>&1
node_start primary || bail "the primary did not restart after the crash"
check "primary after a crash: the journal records and the read are kept" test "$(sql primary "SELECT count(*) FROM sift._test_col_log_entries('ts_journal') WHERE kind IN (4, 5)")|$(scope_rows primary)" == "$RECS|$(join_rows primary)"
check "two paths, primary after a crash: the records of the three journals and the reads are kept, each stamp vouches or says why" test "$(journal_records primary)|$(twohop_reads primary)" == "$JREC|same" -a -n "$(twohop_vouch primary)"
diag "stamps after the crash: $(twohop_vouch primary); the read says: $(twohop_why primary)"
finish
