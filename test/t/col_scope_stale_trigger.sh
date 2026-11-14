#!/usr/bin/env bash
# Pairs that churn on an entity table nobody writes: the reads that find the changed members (they patch them, or the join costs less) ask for the rewrite when the `stale scope` rule says
# it is due, so the scope units are filled again with no VACUUM of the entity table, no REINDEX and no refused read; and once they are filled the reads ask for nothing.
. "$(dirname "$0")/lib.bash"
plan 7

node_init st 54 "autovacuum = off
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.scope_max_multiplicity = 8"
node_start st || bail "start failed"
sql st "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 2000) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 2100, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 200000) i" \
	>/dev/null || bail "setup failed"
# the VACUUM comes in a session of its own, after the load's counters are flushed: the entity table is then quiet, as a table nobody writes is, and the pairs are what changes
sql st "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql st "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (key_order = 'm, d', scope = 't: m -> cp(m, t)')" >/dev/null || bail "build failed"

READ="SELECT count(*), sum(v) FROM ce e WHERE sift.in_scope('ce_col'::regclass, 't', e.m, 6)"
JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 6)"
same() { [[ $(sql st "SET sift.agg_force = on" "$READ" | tail -n 1) == $(sql st "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }
plan_of() { sql st "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $READ"; }
# the scope segments are the immutable ones the build wrote after the entity segment: a rewrite replaces them (a new generation)
gens() { sql st "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ce_col') WHERE kind = 'immutable'" | tail -n 1; }
due() { sql st "SELECT coalesce(sift.col_maintenance_due('ce_col'), '-')" | tail -n 1; }

G0=$(gens)
check "the first build is filled and the rule has nothing to say" test "$(due)" == -
# a third of the pairs end and as many are granted to other members: well over sift.col_scope_refresh_pct of them, and no change to the entity table
sql st "WITH d AS (DELETE FROM cp WHERE (m + t) % 3 = 0 RETURNING m, t) INSERT INTO cp SELECT 1 + (m + 37) % 2000, t FROM d ON CONFLICT DO NOTHING" >/dev/null
check "the rule says stale scope" test "$(due)" == "stale scope"
check "the reads are the join's with the changed pairs" same
refilled() { same >/dev/null; [[ $(gens) != "$G0" ]]; }
check "the reads that find the changed pairs ask for the rewrite: the scope segments are written again within 60 s, with no VACUUM of the entity table" wait_for refilled 60
check "... and the read is from the units with no changed key" test "$(plan_of | grep -c 'Scope Changed Keys: [1-9]')" == 0
check "... with the same answer as the join" same
# the merge of the scope runs the rewrite wrote comes first; after it the reads of the filled index find nothing changed, so no read asks again (a read that asked for what it just
# got would write the segments for ever)
wait_stable st "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ce_col') WHERE kind = 'immutable'" 60
G1=$(gens)
for _ in 1 2 3 4 5 6 7 8 9 10; do same >/dev/null; sleep 0.5; done	# a negative: the reads of 5 s, 25 naptimes, are the observation
check "the reads of the filled index ask for no rewrite" test "$(gens)" == "$G1" -a "$(due)" == -
finish
