#!/usr/bin/env bash
# A scope with a text and a uuid key on a physical standby (test/t/col_scope_replica.sh is the model): the sidecars are keyed by codes of the value store of the key column, which the standby replays. The standby
# reads each key through the runs (Scope Sidecar Runs) and gets the join's rows when the primary edits the pairs and the facts, merges, and builds the index again with new keys that the store of the standby had not
# seen.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"

plan 6
node_init scp 49 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
$SIDE_CONF"
node_start scp || bail "primary start failed"
sql scp "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE pairs_t (dim_a_id int NOT NULL, tenant_id text NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"CREATE TABLE pairs_u (dim_a_id int NOT NULL, tenant_id uuid NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO pairs_t SELECT m, 'tenant-' || (1 + m % 3) FROM generate_series(1, 60) m" \
	"INSERT INTO pairs_u SELECT dim_a_id, md5(tenant_id)::uuid FROM pairs_t" \
	"CREATE INDEX pairs_t_journal ON pairs_t USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE INDEX pairs_u_journal ON pairs_u USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i" \
	"VACUUM (FREEZE, ANALYZE) events, pairs_t, pairs_u" \
	"CREATE INDEX events_t ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> pairs_t(dim_a_id, tenant_id)')" \
	"CREATE INDEX events_u ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> pairs_u(dim_a_id, tenant_id)')" \
	"CREATE FUNCTION answer(variant text, k text, use_scope bool) RETURNS text LANGUAGE plpgsql AS \$\$
	DECLARE r text; key text := CASE variant WHEN 'u' THEN format('md5(%L)::uuid', k) ELSE quote_literal(k) END;
	BEGIN
		PERFORM set_config('sift.enable_agg', use_scope::text, true);
		PERFORM set_config('sift.test_scope_entity', use_scope::text, true);
		EXECUTE format('SELECT array_agg(t ORDER BY kind)::text FROM (SELECT kind, count(*) n, sum(v) s FROM events e
			WHERE e.dim_a_id IN (SELECT dim_a_id FROM pairs_%s WHERE tenant_id = %s) GROUP BY kind) t', variant, key) INTO r;
		RETURN r;
	END \$\$" \
	"CREATE FUNCTION runs_of(variant text, k text) RETURNS int LANGUAGE plpgsql AS \$\$
	DECLARE l text; r int := 0; key text := CASE variant WHEN 'u' THEN format('md5(%L)::uuid', k) ELSE quote_literal(k) END;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'on', true);
		PERFORM set_config('sift.test_scope_entity', 'on', true);
		FOR l IN EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT kind, count(*), sum(v) FROM events e
			WHERE e.dim_a_id IN (SELECT dim_a_id FROM pairs_%s WHERE tenant_id = %s) GROUP BY kind', variant, key) LOOP
			IF l ~ 'Scope Sidecar Runs' THEN r := regexp_replace(l, '^.*: ', '')::int; END IF;
		END LOOP;
		RETURN r;
	END \$\$" \
	"CREATE FUNCTION same(k text) RETURNS bool LANGUAGE sql AS
	\$\$ SELECT answer('t', k, true) IS NOT DISTINCT FROM answer('t', k, false) AND answer('u', k, true) IS NOT DISTINCT FROM answer('u', k, false) \$\$" \
	>/dev/null || bail "setup failed"

node_clone scp scs 50
node_start scs || bail "standby start failed"
ok "standby streaming from a base backup"

KEYS="'tenant-1','tenant-2','tenant-3','tenant-new','tenant-none'"
catchup scp scs
check "a fresh standby reads every key as the join does" test "$(sql scs "SELECT bool_and(same(k)) FROM unnest(ARRAY[$KEYS]) k")" == t
check "the standby takes the runs of a text and a uuid key" test "$(sql scs "SELECT runs_of('t', 'tenant-1') > 0 AND runs_of('u', 'tenant-2') > 0")" == t

sql scp "DELETE FROM pairs_t WHERE dim_a_id IN (3, 9, 12)" "INSERT INTO pairs_t SELECT m, 'tenant-new' FROM generate_series(1, 20) m" \
	"DELETE FROM pairs_u WHERE dim_a_id IN (3, 9, 12)" "INSERT INTO pairs_u SELECT m, md5('tenant-new')::uuid FROM generate_series(1, 20) m" \
	"DELETE FROM events WHERE id % 53 = 0" "UPDATE events SET dim_a_id = 7 WHERE id % 211 = 0" >/dev/null
catchup scp scs
check "after pair and fact edits the standby reads every key as the join does" test "$(sql scs "SELECT bool_and(same(k)) FROM unnest(ARRAY[$KEYS]) k")" == t

sql scp "VACUUM events, pairs_t, pairs_u" >/dev/null
sql scp "SELECT sift.col_promote('events_t'), sift.col_merge('events_t'), sift.col_promote('events_u'), sift.col_merge('events_u')" >/dev/null
catchup scp scs
check "after a merge writes the sidecars with the new key's code the standby reads as the join does" \
	test "$(sql scs "SELECT bool_and(same(k)) FROM unnest(ARRAY[$KEYS]) k")" == t

sql scp "INSERT INTO pairs_t SELECT m, 'tenant-later' FROM generate_series(30, 59) m" "INSERT INTO pairs_u SELECT m, md5('tenant-later')::uuid FROM generate_series(30, 59) m" >/dev/null
sql scp "VACUUM events, pairs_t, pairs_u" >/dev/null
sql scp "REINDEX INDEX events_t" "REINDEX INDEX events_u" >/dev/null
sql scp "VACUUM events, pairs_t, pairs_u" >/dev/null
catchup scp scs
check "after a REINDEX with a key the standby had not seen it reads as the join does, runs taken" \
	test "$(sql scs "SELECT bool_and(same(k)) AND runs_of('t', 'tenant-later') > 0 AND runs_of('u', 'tenant-later') > 0 FROM unnest(ARRAY[$KEYS,'tenant-later']) k")" == t
