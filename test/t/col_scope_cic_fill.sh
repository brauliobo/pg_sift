#!/usr/bin/env bash
# A scope index made with CREATE INDEX CONCURRENTLY starts its journal at the end of its build, so its scope segments carry no stamp and the reads say `unfilled`. The maintenance fills them:
# the reads of a table nobody changes or vacuums ask for the rewrite that does (the table is never vacuumed, and VACUUM is where the maintenance was asked for before), and
# the answers are the join's before and after.
. "$(dirname "$0")/lib.bash"
plan 5

node_init cf 54 "autovacuum = off
sift.col_auto_merge = on
sift.maintenance_naptime = 200
sift.scope_max_multiplicity = 8"
node_start cf || bail "start failed"
sql cf "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 30000) i" \
	"VACUUM (FREEZE, ANALYZE) ce, cp" "CREATE INDEX CONCURRENTLY ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = 't: m -> cp(m, t)')" >/dev/null || bail "setup failed"

READ="SELECT count(*), sum(v) FROM ce e WHERE sift.in_scope('ce_col'::regclass, 't', e.m, 5)"
JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 5)"
why() { sql cf "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $READ" | sed -n 's/^ *Scope Fallback: //p;s/^ *Scope Units: .*/stored/p' | head -n 1; }
same() { [[ $(sql cf "SET sift.agg_force = on" "$READ" | tail -n 1) == $(sql cf "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

out=$(why); check "right after CREATE INDEX CONCURRENTLY the read says unfilled" test "$out" == unfilled
check "... and its answer is the join's" same
# the reads are what asks for the maintenance (a table that opted out of autovacuum is left alone): they go on while the worker runs
filled() { why >/dev/null; [[ $(why) == stored ]]; }
check "the reads of a table that is never changed or vacuumed read the scope units within 60 s" wait_for filled 60
check "... with the same answer as the join" same
check "... and no key differs from the join" test "$(sql cf "SELECT count(*) FROM sift.col_scope_verify('ce_col', 1000) WHERE problem <> 'unverified'" | tail -n 1)" == 0
finish
