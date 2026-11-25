#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# An UPDATE that changes no leaf of the index but moves the row to another heap address is a record of the column delta chain: the part keeps the address the row had, and the delete of the row
# that follows names it by the new one. The summary read of a scope matched the pending deletes with the addresses of the part, so a deleted row stayed in the cubes of every snapshot that sees
# the delete, until the deletion vector of its part took it: a REPEATABLE READ read of the summary counted rows the heap had ended.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 2
node_init rv 96 "autovacuum = off
sift.maintenance_naptime = 60000${SIFT_TESTING:+
sift.test_create_format1 = off}"
node_start rv || bail "start failed"
SCOPE="scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)'"
READ="SELECT count(*), sum(v) FROM events e JOIN tenant_scope p ON p.dim_a_id = e.dim_a_id WHERE p.tenant_id = 12"
sql rv "CREATE EXTENSION pg_sift" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 2) j ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text)" \
	"INSERT INTO events SELECT i, 1 + i % 300, timestamptz '2026-01-01' + i * interval '1 minute', i % 10, 'a' FROM generate_series(1, 9000) i" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" \
	"UPDATE events SET pad = repeat('b', 300) WHERE id BETWEEN 100 AND 130" \
	"DELETE FROM events WHERE id BETWEEN 100 AND 115" >/dev/null || bail "setup failed"

out=$(sql rv "SET sift.enable_agg = on" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $READ")
check "the read of the scope is served from its summary" test "$(grep -c 'Scope Read: summary' <<<"$out")" == 1
SCOPE_ANSWER=$(sql rv "BEGIN ISOLATION LEVEL REPEATABLE READ" "SET sift.enable_agg = on" "$READ" "COMMIT")
PLAIN_ANSWER=$(sql rv "BEGIN ISOLATION LEVEL REPEATABLE READ" "SET sift.enable_agg = off" "$READ" "COMMIT")
diag "summary: $SCOPE_ANSWER, heap: $PLAIN_ANSWER"
check "the summary read counts the rows the heap shows after the deletes of rows that moved" test "$SCOPE_ANSWER" == "$PLAIN_ANSWER"
finish
