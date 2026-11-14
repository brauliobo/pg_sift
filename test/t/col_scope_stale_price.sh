#!/usr/bin/env bash
# Pairs that churn a little (1%, under sift.col_scope_refresh_pct) on an entity table nobody writes: the reads pay for the changed members on top of the filled read, and the rewrite is asked for
# when what they paid comes to half of its price. No read, no rewrite; a few reads, a few naptimes apart or together, are not enough; a burst of them is, and what they paid is not owed to the
# segments the rewrite replaced.
. "$(dirname "$0")/lib.bash"
plan 7

node_init sp 56 "autovacuum = off
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.scope_max_multiplicity = 8
sift.test_scope_patch = off"
node_start sp || bail "start failed"
sql sp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 2000) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 2100, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 200000) i" \
	>/dev/null || bail "setup failed"
sql sp "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql sp "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (key_order = 'm, d', scope = 't: m -> cp(m, t)')" >/dev/null || bail "build failed"

JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 6)"
# n reads of the statement the application writes, in one session; the planner decides how each is read (the join, for the members that changed)
reads() { sql sp "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "DO \$\$ BEGIN FOR i IN 1..$1 LOOP PERFORM ${JOIN#SELECT }; END LOOP; END \$\$" >/dev/null; }
same() { [[ $(sql sp "SET sift.agg_force = on" "$JOIN" | tail -n 1) == $(sql sp "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }
gens() { sql sp "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ce_col') WHERE kind = 'immutable'" | tail -n 1; }
due() { sql sp "SELECT coalesce(sift.col_maintenance_due('ce_col'), '-')" | tail -n 1; }
paid() { sql sp "SELECT sift._test_scope_paid('ce_col', 0)" | tail -n 1; }
# the rewrite asks for the pairs the reads found changed: wait a few naptimes (25 of them) and see that none came
quiet() { for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do sleep 0.2; done; }

G0=$(gens)
# 1% of the pairs end and as many are granted to other members: far under sift.col_scope_refresh_pct, and no change to the entity table
sql sp "WITH d AS (DELETE FROM cp WHERE abs(hashtext(m || '/' || t || '/b')) % 100 < 2 RETURNING m, t) INSERT INTO cp SELECT 1 + (m + 37) % 2000, t FROM d ON CONFLICT DO NOTHING" >/dev/null
quiet
check "no read: the rule has nothing to say and the segments are as they were" test "$(due)" == - -a "$(gens)" == "$G0"
reads 20
check "20 reads in one go pay for the changed dim_b, a fraction of the rewrite" test "$(paid)" -gt 0
quiet
check "... which is not enough: the rule says nothing and the segments are as they were" test "$(due)" == - -a "$(gens)" == "$G0"
for _ in 1 2 3 4 5 6 7 8 9 10; do reads 1; sleep 0.2; done
quiet
check "10 reads, one for each naptime, are not enough either" test "$(due)" == - -a "$(gens)" == "$G0"
check "the reads are the join's: the answer is the join's" same
refilled() { [[ $(gens) != "$G0" ]]; }
# a burst: the rewrite costs 1.1 G instructions here and a read pays a few million for the members of the key that changed: a few hundred of them pay half of it
for _ in 1 2 3 4 5 6 7 8; do reads 100; done
check "a burst of reads asks for the rewrite: the scope segments are written again within 60 s" wait_for refilled 60
wait_stable sp "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ce_col') WHERE kind = 'immutable'" 60
G1=$(gens)
reads 50
quiet
check "what the reads paid is not owed to the new segments: the reads of the filled index ask for no rewrite" test "$(paid)" -eq 0 -a "$(due)" == - -a "$(gens)" == "$G1"
finish
