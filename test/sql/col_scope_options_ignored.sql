-- cols(..) and max_multiplicity(..) of a scope declaration were options of the scope segments of 5.0, which kept a copy of the fact rows for each scope key. The scope of 5.1 keeps no copy (it holds runs
-- of the units of the index, which keep every column), so a declaration that has them is accepted and CREATE INDEX and REINDEX say that they are ignored; the rows a scope read gives
-- are the join's, and the index is as an index without them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT i, 1 + i % 5 FROM generate_series(1, 500) i;
INSERT INTO tenant_scope SELECT i, 6 FROM generate_series(1, 500, 7) i;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind int NOT NULL, value_num int NOT NULL)
	WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 500, timestamptz '2026-01-01' + i * interval '1 minute', i % 7, i FROM generate_series(1, 3000) i;
CREATE FUNCTION answers(sql text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE r text; BEGIN EXECUTE format('SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (%s) t', sql) INTO r; RETURN r; END $$;

CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(kind) max_multiplicity(1)');
ALTER INDEX events_col SET (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(value_num)');
REINDEX INDEX events_col;
SELECT reloptions FROM pg_class WHERE oid = 'events_col'::regclass;

SET sift.enable_agg = on;
SELECT answers('SELECT kind, count(*), sum(value_num) FROM events e JOIN tenant_scope s USING (dim_a_id) WHERE s.tenant_id = ' || k || ' GROUP BY kind') =
       (SELECT answers('SELECT kind, count(*), sum(value_num) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ' || k || ') GROUP BY kind')) AS same_rows
  FROM generate_series(1, 6) k;
SELECT sift.col_verify('events_col', true);

DROP TABLE events, tenant_scope;
DROP FUNCTION answers(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
