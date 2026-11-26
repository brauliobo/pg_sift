#!/usr/bin/env bash
# tap: pooled
# A REINDEX of the journal index of the pairs is another lineage: the records of the pairs changed before it were in the old one. The summary read of a Format 2.0 scope does not lose them: it
# patches the members that changed since its stamps in from the pairs (Scope Cube: delta), so it stays the read, with the answers of the join, before and after the REINDEX, after the maintenance
# and with pairs written later. The index is of Format 2.0 on both runs: a Format 1 index has no scope read of its own.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 7

node_init jr 53 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
sift.test_create_format1 = off"
node_start jr || bail "start failed"
sql jr "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 30000) i" \
	>/dev/null || bail "setup failed"
# the VACUUM comes in a session of its own, after the load's counters are flushed: the table is then quiet (nothing changed since its VACUUM), as a table nobody writes is,
# so nothing but the reads of the scope index could have asked for a rewrite
sql jr "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql jr "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = 't: m -> cp(m, t) max_multiplicity(8)')" >/dev/null || bail "build failed"

JOIN="SELECT count(*), sum(v) FROM ce e JOIN cp p ON p.m = e.m WHERE p.t = 6"
why() { sql jr "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $JOIN" | sed -n 's/^ *Scope Read: //p' | grep . || echo "no scope read"; }
same() { [[ $(sql jr "$JOIN" | tail -n 1) == $(sql jr "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

check "the first build is filled: the read is from the summary ($(why))" test "$(why)" == summary
sql jr "INSERT INTO cp SELECT m, 6 FROM generate_series(1, 60) m ON CONFLICT DO NOTHING" "DELETE FROM cp WHERE t = 6 AND m > 150" >/dev/null
sql jr "REINDEX INDEX cp_j" >/dev/null
check "after pairs changed and a REINDEX of the journal the read is still from the summary ($(why))" test "$(why)" == summary
check "... and its answer is the join's" same
sql jr "VACUUM ce" "SELECT sift.maintain('ce_col')" >/dev/null
check "after the maintenance the read is from the summary ($(why))" test "$(why)" == summary
check "... with the same answer as the join" same
sql jr "INSERT INTO cp SELECT 201 + i, 6 FROM generate_series(1, 5) i" >/dev/null
check "a pair written after it is patched into the read, which is the join's" same
check "... and the index verifies" test -z "$(sql jr "SELECT sift.col_verify('ce_col', true)" 2>&1 | grep -v '^$')"
finish
