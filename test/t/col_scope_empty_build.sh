#!/usr/bin/env bash
# A scope index built on an empty table has no segment, so its reads say `unfilled` (the join answers) while the rows that arrive are in the write buffers; the promotion of the buffers writes the
# segments with the stamp the journal of the scope gives at that moment, so the first promotion makes the reads stored with no refill, and the answers are the join's before and after.
. "$(dirname "$0")/lib.bash"
plan 5

node_init eb 55 "autovacuum = off
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.scope_max_multiplicity = 8"
node_start eb || bail "start failed"
sql eb "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = 't: m -> cp(m, t)')" >/dev/null || bail "setup failed"

READ="SELECT count(*), sum(v) FROM ce e WHERE sift.in_scope('ce_col'::regclass, 't', e.m, 5)"
JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 5)"
why() { sql eb "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $READ" | sed -n 's/^ *Scope Fallback: //p;s/^ *Scope Units: .*/stored/p' | head -n 1; }
same() { [[ $(sql eb "SET sift.agg_force = on" "$READ" | tail -n 1) == $(sql eb "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

check "on an empty table the read says unfilled" test "$(why)" == unfilled
sql eb "INSERT INTO ce SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 30000) i" "VACUUM ce" >/dev/null
check "... and its answer is the join's once rows arrive" same
sql eb "SELECT sift.col_promote('ce_col')" >/dev/null
check "the first promotion of its rows makes the reads read the scope units, with no refill" test "$(why)" == stored
check "... with the same answer as the join" same
check "... and no key differs from the join" test "$(sql eb "SELECT count(*) FROM sift.col_scope_verify('ce_col', 1000) WHERE problem <> 'unverified'" | tail -n 1)" == 0
finish
