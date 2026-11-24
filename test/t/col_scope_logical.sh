#!/usr/bin/env bash
# A scope index on the subscriber of a logical replication, in Format 1 and Format 2.0: the apply worker writes the facts and the pairs (the scope relation), the subscription is created after the scope index, and the pairs
# arrive, change and go after the index was built. The subscriber cannot hear the apply worker's deletes (no statement trigger), so its scope read is declined and the aggregates of the facts run on the join of the pairs
# journal: every read of the scope index on the subscriber, through the units and the join (counts, ordered string_aggs of the columns of both joined tables cast to text), is the publisher's plain answer, and the index verifies there
# (sift.col_scope_verify on Format 1, sift.check on Format 2.0).
. "$(dirname "$0")/lib.bash"

plan 5
node_init scl_p 35 "wal_level = logical
max_replication_slots = 5
max_wal_senders = 5
autovacuum = off"
node_start scl_p || bail "publisher start failed"
node_init scl_s 59 "wal_level = logical
max_replication_slots = 5
max_logical_replication_workers = 4
max_worker_processes = 16
autovacuum = off"
node_start scl_s || bail "subscriber start failed"
export PGOPTIONS="-c client_min_messages=warning"

FORMAT1=(on off)
for n in 1 2; do
	TABLES="CREATE TABLE facts$n (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, amount int NOT NULL, pad text DEFAULT repeat('x', 1500)) WITH (autovacuum_enabled = false)
CREATE TABLE pairs$n (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)"
	while IFS= read -r q; do sql scl_p "$q" >/dev/null && sql scl_s "$q" >/dev/null || bail "ddl failed: $q"; done <<<"$TABLES"
	sql scl_p "INSERT INTO facts$n SELECT i, 1 + i % 120, timestamptz '2026-01-01' + i * interval '1 minute', i % 100 FROM generate_series(1, 4000) i" >/dev/null || bail "publisher data failed"
	sql scl_s "CREATE EXTENSION IF NOT EXISTS pg_sift" "SET sift.test_create_format1 = ${FORMAT1[n - 1]}" \
		"CREATE INDEX pairs${n}_journal ON pairs$n USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
		"CREATE INDEX facts${n}_col ON facts$n USING sift_col (id, dim_a_id, created_at, amount) WITH (key_order = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> pairs$n(dim_a_id, tenant_id) max_multiplicity(64)')" \
		>/dev/null || bail "subscriber index failed"
done
sql scl_p "CREATE PUBLICATION pb FOR TABLE facts1, pairs1, facts2, pairs2" >/dev/null || bail "publication failed"
sql scl_s "CREATE SUBSCRIPTION sb CONNECTION '$(conninfo scl_p)' PUBLICATION pb" >/dev/null || bail "subscription failed"

CONTENT="SELECT (SELECT sum(id) FROM facts1) || ':' || (SELECT count(*) FROM pairs1) || ':' || (SELECT sum(id) FROM facts2) || ':' || (SELECT count(*) FROM pairs2)"
synced() { [[ $(sql scl_p "$CONTENT") == "$(sql scl_s "$CONTENT")" ]]; }
wait_for synced 60 || bail "the subscriber has no facts"
for n in 1 2; do
	sql scl_p "INSERT INTO pairs$n SELECT m, 1 + (m * 3 + j * 7) % 10 FROM generate_series(1, 120) m, generate_series(0, 5) j WHERE j <= m % 6 ON CONFLICT DO NOTHING" >/dev/null
done
check "the pairs that arrive after the scope indexes are on the subscriber" wait_for synced 60
for n in 1 2; do
	sql scl_p "DELETE FROM pairs$n WHERE dim_a_id % 5 = 0" "UPDATE pairs$n SET tenant_id = 1 + (tenant_id + 3) % 10 WHERE dim_a_id % 7 = 0" "DELETE FROM facts$n WHERE id % 13 = 0" \
		"INSERT INTO pairs$n SELECT m, 1 + (m * 3) % 10 FROM generate_series(1, 120) m ON CONFLICT DO NOTHING" \
		"INSERT INTO facts$n SELECT i, 1 + i % 120, timestamptz '2026-03-01' + i * interval '1 minute', i % 100 FROM generate_series(4001, 4400) i" >/dev/null
done
check "the deleted, moved and new rows are on the subscriber" wait_for synced 60
sql scl_s "ANALYZE" >/dev/null

DIGEST="SELECT md5(string_agg(m || ':' || n || ':' || tenants || ':' || ids, ';' ORDER BY m)) FROM (SELECT f.dim_a_id m, count(*) n, string_agg(p.tenant_id::text, ',' ORDER BY p.tenant_id) tenants,
	string_agg(f.id::text, ',' ORDER BY f.id) ids FROM facts%d f JOIN pairs%d p ON p.dim_a_id = f.dim_a_id GROUP BY 1) q"
digest() { # NODE TABLE-NUMBER ENGINE
	sql "$1" "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $3" "SET sift.agg_force = $3" "$(printf "$DIGEST" "$2" "$2")" | tail -n 1
}
check "the ordered string_aggs of the columns of both joined tables cast to text on the subscriber is the publisher's plain answer (Format 1)" test "$(digest scl_s 1 on)" == "$(digest scl_p 1 off)"
check "the ordered string_aggs of the columns of both joined tables cast to text on the subscriber is the publisher's plain answer (Format 2.0)" test "$(digest scl_s 2 on)" == "$(digest scl_p 2 off)"
check "the scope indexes verify on the subscriber (Format 1: sift.col_scope_verify, Format 2.0: sift.check)" \
	test "$(sql scl_s "LOAD 'pg_sift'" "SELECT count(*) FROM sift.col_scope_verify('facts1_col', 50) WHERE problem = 'differs'" | tail -n 1)|$(sql scl_s "LOAD 'pg_sift'" "SELECT count(*) FROM sift.check('facts2_col', true, 50)" | tail -n 1)" == "0|0"
finish
