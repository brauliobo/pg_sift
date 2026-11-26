#!/usr/bin/env bash
# tap: pooled
# A scope index built on an empty table has no part, but it has its stamps (the journal at the end of the build), so its reads are from the summary from the start (none to read: the rows that
# arrive are staged or in slabs, which the read patches in) and the VACUUM that settles its rows writes the summary with no refill; the answers are the join's before and after.
# The index is of Format 2.0 on both runs: a Format 1 index has no scope read of its own.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 5

node_init eb 55 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
sift.test_create_format1 = off"
node_start eb || bail "start failed"
sql eb "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (scope = 't: m -> cp(m, t) max_multiplicity(8)')" >/dev/null || bail "setup failed"

JOIN="SELECT count(*), sum(v) FROM ce e JOIN cp p ON p.m = e.m WHERE p.t = 5"
why() { sql eb "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $JOIN" | sed -n 's/^ *Scope Read: //p' | grep . || echo "no scope read"; }
same() { [[ $(sql eb "$JOIN" | tail -n 1) == $(sql eb "SET sift.enable_agg = off" "$JOIN" | tail -n 1) ]]; }

check "on an empty table the read is from the summary ($(why))" test "$(why)" == summary
sql eb "INSERT INTO ce SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 30000) i" "VACUUM ce" >/dev/null
check "... and its answer is the join's once rows arrive" same
sql eb "VACUUM ce" "SELECT sift.maintain('ce_col')" >/dev/null
check "once its rows are parts the reads are from the summary, with no refill ($(why))" test "$(why)" == summary
check "... with the same answer as the join" same
check "... and the index verifies" test -z "$(sql eb "SELECT sift.col_verify('ce_col', true)" 2>&1 | grep -v '^$')"
finish
