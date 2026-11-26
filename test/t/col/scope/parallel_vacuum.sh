#!/usr/bin/env bash
# A VACUUM whose index cleanup runs in parallel workers (the leader cleans up its indexes inside the parallel operation) compacts the parts of a Format 2.0 scope index and rebuilds the summary of
# the scope for them: the statement completes, the index verifies and its reads are the join's.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 3

node_init pv 61 "autovacuum = off
sift.test_create_format1 = off"
node_start pv || bail "start failed"
sql pv "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE pairs (m int NOT NULL, t int NOT NULL, PRIMARY KEY (m, t))" \
	"INSERT INTO pairs SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 5) j WHERE j < m % 7 ON CONFLICT DO NOTHING" \
	"CREATE INDEX pairs_j ON pairs USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE facts (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO facts SELECT i, 1 + (i * 7919) % 420, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 8000) i" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, m, d, v) WITH (scope = 't: m -> pairs(m, t) max_multiplicity(64)', key_order = 'm, d')" \
	"CREATE INDEX facts_m ON facts (m)" "CREATE INDEX facts_d ON facts (d)" "DELETE FROM facts WHERE id % 5 = 1" >/dev/null || bail "setup failed"

vacuum() { PGOPTIONS="-c min_parallel_index_scan_size=0" psql -X -At -h "$(dir pv)/sock" -p "$(port pv)" -U postgres -c "VACUUM (VERBOSE) facts" 2>&1; }
READ="SELECT count(*), sum(v) FROM facts e WHERE sift.in_scope('facts_col'::regclass, 't', e.m, 5)"
JOIN="SELECT count(*), sum(v) FROM facts e WHERE e.m IN (SELECT m FROM pairs WHERE t = 5)"

out=$(vacuum)
check "VACUUM cleans up the indexes in parallel workers and completes" test "$(grep -c 'launched [0-9]* parallel vacuum workers' <<<"$out")" == 1 -a "$(grep -c ERROR <<<"$out")" == 0
check "the scope index verifies" test "$(sql pv "SELECT count(*) FROM sift.check('facts_col', true, 50)")" == 0
check "its reads are the join's" test "$(sql pv "SET sift.agg_force = on" "$READ" | tail -n 1)" == "$(sql pv "SET sift.enable_agg = off" "$JOIN" | tail -n 1)"
finish
