-- The entries of a sift_col index lead to the first version of a HOT chain, as the entries of every index do: an UPDATE that keeps the indexed columns links the new version to the old one, and the
-- address of the row stays the one of the first version (a redirect once VACUUM has pruned it), not the address the heap scan of the table reports for the live version. The scope summary counts
-- each row of the entity parts once whatever built them (CREATE INDEX, a promotion, a pair that changes) and whatever happened to the table since: a scope read is the join.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
\ir test/common/sql/col/scope/base.sql

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 5) j WHERE j < m % 7 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false, fillfactor = 50);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 4000) i;
-- the column no index holds: the update is a HOT one and the rows it touches are chains
UPDATE events SET pad = 'y' WHERE id % 3 = 0;

-- the join of one key or of a set of keys (a row for each pair), and IN of a set (a fact once): whether the rows are the same and how the node read them
CREATE FUNCTION reads(OUT n int, OUT same boolean, OUT route text) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	k record;
BEGIN
	FOR k IN SELECT * FROM (VALUES (1, '{1}', true), (2, '{5}', true), (3, '{12}', true), (4, '{2,9}', true), (5, '{1,3,5,7,9,11}', false)) v(n, keys, pairs) LOOP
		SELECT r.same, r.route INTO same, route
		FROM scope_read(CASE WHEN k.pairs
			THEN format('SELECT count(*), sum(v), min(id), max(id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = ANY (%L::int8[])', k.keys)
			ELSE format('SELECT count(*), sum(v), min(id), max(id), count(DISTINCT dim_a_id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ANY (%L::int8[]))', k.keys) END) r;
		n := k.n;
		RETURN NEXT;
	END LOOP;
END $$;

-- a build over chains
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
SELECT * FROM reads();

-- more chains and a pair that changes and a member that moves: the rows of a member are keyed by the addresses of the entity segments, chains included
UPDATE events SET pad = 'z' WHERE id % 5 = 0;
INSERT INTO tenant_scope SELECT m, 1 + (m + 5) % 12 FROM generate_series(1, 300) m WHERE m % 4 = 0 ON CONFLICT DO NOTHING;
DELETE FROM tenant_scope WHERE dim_a_id % 6 = 1;
SELECT * FROM reads();
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);

-- new rows, a promotion, the old ones updated again (a chain of three versions) and VACUUM prunes them
INSERT INTO events SELECT i, 1 + i % 320, timestamptz '2026-06-01' + i * interval '1 minute', i % 10, 'x' FROM generate_series(4001, 5000) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
UPDATE events SET pad = 'w' WHERE id % 3 = 0;
VACUUM events;
SELECT * FROM reads();
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);

-- a rebuild over the chains
REINDEX INDEX events_col;
SELECT * FROM reads();
SELECT sift.col_verify('events_col', true);

DROP TABLE events, tenant_scope;
DROP FUNCTION reads();
DROP FUNCTION scope_run(text, text), scope_group_run(text), scope_reason_ok(text), scope_read(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
