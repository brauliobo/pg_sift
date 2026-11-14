#!/usr/bin/env bash
# A REINDEX of the journal index of the pairs is another lineage: the stamps of the scope segments are of the old one, so the reads say `other lineage` (the join answers, exactly) and the
# maintenance sees the segments as stale. The reads ask for the rewrite that fills them again (a table nobody vacuums gets it too), so the units are read within seconds with no REINDEX of the
# scope index, and the answers are the join's before, during and after.
. "$(dirname "$0")/lib.bash"
plan 7

node_init jr 53 "autovacuum = off
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.scope_max_multiplicity = 8"
node_start jr || bail "start failed"
sql jr "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 30000) i" \
	>/dev/null || bail "setup failed"
# the VACUUM comes in a session of its own, after the load's counters are flushed: the table is then quiet (nothing changed since its VACUUM), as a table nobody writes is,
# so nothing but the reads of the scope index can ask for the rewrite
sql jr "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql jr "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = 't: m -> cp(m, t)')" >/dev/null || bail "build failed"

READ="SELECT count(*), sum(v) FROM ce e WHERE sift.in_scope('ce_col'::regclass, 't', e.m, 5)"
JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 5)"
why() { sql jr "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $READ" | sed -n 's/^ *Scope Fallback: //p;s/^ *Scope Units: .*/stored/p' | head -n 1; }
same() { [[ $(sql jr "SET sift.agg_force = on" "$READ" | tail -n 1) == $(sql jr "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

check "the first build is filled: the read is from the scope units" test "$(why)" == stored
sql jr "REINDEX INDEX cp_j" >/dev/null
check "after a REINDEX of the journal the first read says other lineage" test "$(why)" == "other lineage"
check "... and its answer is the join's" same
filled() { why >/dev/null; [[ $(why) == stored ]]; }
check "the reads ask for the rewrite and read the scope units again within 60 s, with no REINDEX of the scope index" wait_for filled 60
check "... with the same answer as the join" same
sql jr "INSERT INTO cp SELECT 201 + i, 5 FROM generate_series(1, 5) i" >/dev/null
check "a pair written after it is patched into the read, which is the join's" same
check "... and no key differs from the join" test "$(sql jr "SELECT count(*) FROM sift.col_scope_verify('ce_col', 1000) WHERE problem <> 'unverified'" | tail -n 1)" == 0
finish
