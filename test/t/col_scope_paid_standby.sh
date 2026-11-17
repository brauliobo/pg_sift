#!/usr/bin/env bash
# A standby cannot write, and the only things it sends the primary are the positions of its replay and the horizon of its snapshots (the receiver's status and feedback messages): what its reads pay for the
# pairs changed since the floor of a scope index reaches nobody. What it has is the primary's sum, replayed from the log (the metapage), and the benefit of the primary's rewrite, which it replays. So: the standby
# answers as the primary's join does, its reads do not move the primary's sum, the primary's sum is the standby's after the replay, the primary's reads make the rewrite due and start none, and the rewrite the user runs is read on the standby from the units.
. "$(dirname "$0")/lib.bash"
plan 7

node_init pp 58 "autovacuum = off
wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.scope_max_multiplicity = 8
sift.test_scope_patch = off"
node_start pp || bail "primary start failed"
sql pp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 2000) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 2100, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 200000) i" \
	>/dev/null || bail "setup failed"
sql pp "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql pp "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (key_order = 'm, d', scope = 't: m -> cp(m, t)')" >/dev/null || bail "build failed"
sql pp "WITH d AS (DELETE FROM cp WHERE abs(hashtext(m || '/' || t || '/b')) % 100 < 2 RETURNING m, t) INSERT INTO cp SELECT 1 + (m + 37) % 2000, t FROM d ON CONFLICT DO NOTHING" "CHECKPOINT" >/dev/null

node_clone pp ps 59 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.test_scope_patch = off"
node_start ps || bail "standby start failed"

JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 6)"
reads() { sql "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "DO \$\$ BEGIN FOR i IN 1..$2 LOOP PERFORM ${JOIN#SELECT }; END LOOP; END \$\$" >/dev/null; }
paid() { sql "$1" "SELECT paid FROM sift._test_scope_balance('ce_col')" | tail -n 1; }
half() { sql pp "SELECT half::int8 FROM sift._test_scope_balance('ce_col')" | tail -n 1; }
gens() { sql "$1" "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ce_col') WHERE kind = 'immutable'" | tail -n 1; }
changed() { sql ps "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $JOIN" | grep -c 'Scope Changed Keys'; }
same() { [[ $(sql ps "SET sift.agg_force = on" "$JOIN" | tail -n 1) == $(sql pp "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

catchup pp ps
G0=$(gens pp)
check "the standby reads the stale index and answers as the primary's join does" same
reads ps 300
check "300 reads on the standby pay for nothing the primary can see: its sum is zero and its segments are as they were" test "$(paid pp)" -eq 0 -a "$(gens pp)" == "$G0"
HALF=$(half)
reads pp 20
PP=$(paid pp)
sql pp "SELECT sift._test_scope_paid_flush('ce_col')" >/dev/null
catchup pp ps
PS=$(paid ps)
out="primary sum $PP, standby sum $PS, half $HALF; primary balance $(sql pp "SELECT * FROM sift._test_scope_balance('ce_col')" | tail -n 1), standby balance $(sql ps "SELECT * FROM sift._test_scope_balance('ce_col')" | tail -n 1)"
check "the primary's sum is replayed: the standby has it" test "$PS" -eq "$PP" -a "$PP" -gt 0
check "and the standby's own reads are not added to it" test "$PS" -lt "$HALF"
out=''

for _ in 1 2 3 4 5 6 7 8; do reads pp 100; done
due() { sql pp "SELECT coalesce(sift.col_maintenance_due('ce_col'), '-')" | tail -n 1; }
check "the primary's reads make the rewrite due and start none: stale scope, the segments as they were" test "$(due)" == "stale scope" -a "$(gens pp)" == "$G0"
sql pp "SELECT sift.col_maintain('ce_col')" >/dev/null
check "the explicit rewrite writes the scope segments again" test "$(gens pp)" != "$G0"
catchup pp ps
check "the standby replays the rewrite: its reads are the filled read, no changed member" test "$(changed)" -eq 0
finish
