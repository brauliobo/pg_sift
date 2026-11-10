#!/usr/bin/env bash
# The journal of the scope relation (col/scope_journal.c) is the tombstone log of its journal index: WAL-logged, so a hot standby has the records of the pairs changed on the primary
# and a crash of the primary keeps them. What must hold on every node whatever the verdict (the standby may say it cannot compare a stamp with its snapshots): the answer of a read of
# the scope units is the answer of the join, and a read after a pair changed is never taken from the stored segments.
. "$(dirname "$0")/lib.bash"

plan 7
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
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
check "primary: a changed member is never read as stored" test "$(scope_why primary)" != stored
check "standby: a changed member is never read as stored" test "$(scope_why standby)" != stored

# a crash of the primary keeps the journal: its records are in the WAL, and the read is still the join
RECS=$(sql primary "SELECT count(*) FROM sift._test_col_log_entries('ts_journal') WHERE kind IN (4, 5)")
pg_ctl -D "$(dir primary)/data" -m immediate stop >/dev/null 2>&1
node_start primary || bail "the primary did not restart after the crash"
check "primary after a crash: the journal records and the read are kept" test "$(sql primary "SELECT count(*) FROM sift._test_col_log_entries('ts_journal') WHERE kind IN (4, 5)")|$(scope_rows primary)" == "$RECS|$(join_rows primary)"
finish
