#!/usr/bin/env bash
# tap: pooled
# A scope index made with CREATE INDEX CONCURRENTLY has no stamp when its first pass is over (the journal of the first scope index of a relation starts at the end of its build, as a concurrent
# build does not hold the writers out), so the end of the build fills the summary from the journal: the reads are from the summary as soon as the index is there, with the answers of the join,
# and nothing waits for the maintenance or a REINDEX. The index is of Format 2.0 on both runs: a Format 1 index has no scope read of its own.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 4

node_init cf 54 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
sift.test_create_format1 = off"
node_start cf || bail "start failed"
sql cf "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 30000) i" \
	"VACUUM (FREEZE, ANALYZE) ce, cp" "CREATE INDEX CONCURRENTLY ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = 't: m -> cp(m, t) max_multiplicity(8)')" >/dev/null || bail "setup failed"

JOIN="SELECT count(*), sum(v) FROM ce e JOIN cp p ON p.m = e.m WHERE p.t = 5"
why() { sql cf "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $JOIN" | sed -n 's/^ *Scope Read: //p' | grep . || echo "no scope read"; }
same() { [[ $(sql cf "$JOIN" | tail -n 1) == $(sql cf "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

check "right after CREATE INDEX CONCURRENTLY the read is from the summary ($(why))" test "$(why)" == summary
check "... and its answer is the join's" same
check "... and the index verifies" test -z "$(sql cf "SELECT sift.col_verify('ce_col', true)" 2>&1 | grep -v '^$')"
# rows that arrive after the build are read from the write buffers, and the answer is still the join's
sql cf "INSERT INTO ce SELECT 30000 + i, 1 + (i * 31) % 210, timestamptz '2026-02-01', i % 10, 'y' FROM generate_series(1, 2000) i" >/dev/null
check "... also for the rows that arrive after it" same
finish
