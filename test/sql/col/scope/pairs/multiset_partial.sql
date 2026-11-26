-- The pairs of a scope are the rows of a PARTIAL btree that is not unique: a bridge table that soft deletes keeps a row for each source of a pair, and a pair exists while at least one row that is not
-- soft deleted holds it. Soft deleting one of two rows keeps the pair, soft deleting both ends it, restoring one brings it back; the journal index is plain and lists the soft deleted rows too, so the
-- pairs of a changed dim_a_id are those of the btree. Table-driven: each state by each read shape, every cell md5 equal through the scope node, sift with sift.agg_scope off and the normal plan
-- over the active rows.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- dim_a_id d is in the key 1 + d % 10; every fifth has a second row (row_src 2) of the pair, every eleventh has its first row soft deleted, every thirteenth both
CREATE TABLE soft_pairs (id serial PRIMARY KEY, row_src int NOT NULL, dim_a_id int NOT NULL, tenant_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO soft_pairs (row_src, dim_a_id, tenant_id, deleted_at) SELECT 1, d, 1 + d % 10, CASE WHEN d % 11 = 0 OR d % 13 = 0 THEN now() END FROM generate_series(1, 300) d;
INSERT INTO soft_pairs (row_src, dim_a_id, tenant_id, deleted_at) SELECT 2, d, 1 + d % 10, CASE WHEN d % 13 = 0 THEN now() END FROM generate_series(1, 300) d WHERE d % 5 = 0;
-- the btree of all the rows is no pairs' btree to choose: the only partial one is
CREATE INDEX soft_pairs_all ON soft_pairs (dim_a_id, tenant_id);
CREATE INDEX soft_pairs_active ON soft_pairs (tenant_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE INDEX soft_pairs_journal ON soft_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
SELECT count(*) FILTER (WHERE deleted_at IS NULL) AS active_rows, count(DISTINCT (dim_a_id, tenant_id)) FILTER (WHERE deleted_at IS NULL) AS active_pairs, count(*) AS rows FROM soft_pairs;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 100 < 15 THEN NULL ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 6000) i;
SET client_min_messages = notice;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> soft_pairs(dim_a_id, tenant_id)');
SET client_min_messages = warning;
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) soft_pairs;

\ir test/common/sql/col/scope/multiset.sql

CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'built', 'SELECT 1'),
	(2, 'one of two active rows soft deleted: the pair stays', 'UPDATE soft_pairs SET deleted_at = now() WHERE id IN (SELECT min(id) FROM soft_pairs WHERE dim_a_id IN (5, 15) AND deleted_at IS NULL GROUP BY dim_a_id, tenant_id)'),
	(3, 'both active rows soft deleted: the pair is gone', 'UPDATE soft_pairs SET deleted_at = now() WHERE dim_a_id IN (20, 25) AND deleted_at IS NULL'),
	(4, 'one soft deleted row restored: the pair is back', 'UPDATE soft_pairs SET deleted_at = NULL WHERE id IN (SELECT min(id) FROM soft_pairs WHERE dim_a_id IN (20, 11) GROUP BY dim_a_id)'),
	(5, 'a second active row of a pair whose only row is soft deleted: the pair is back', 'INSERT INTO soft_pairs (row_src, dim_a_id, tenant_id) VALUES (3, 22, 3), (3, 33, 4), (3, 33, 4)'),
	(6, 'a duplicate active row inserted and one soft deleted in the same statement', 'WITH d AS (INSERT INTO soft_pairs (row_src, dim_a_id, tenant_id) SELECT 4, dim_a_id, tenant_id FROM soft_pairs WHERE dim_a_id IN (40, 50) AND deleted_at IS NULL RETURNING 1) UPDATE soft_pairs SET deleted_at = now() WHERE dim_a_id = 40 AND row_src = 1'),
	(7, 'a row deleted, not soft deleted: the other keeps the pair', 'DELETE FROM soft_pairs WHERE id IN (SELECT min(id) FROM soft_pairs WHERE dim_a_id IN (30, 35) AND deleted_at IS NULL GROUP BY dim_a_id, tenant_id HAVING count(*) > 1)'),
	(8, 'the key of a soft deleted row changed: still no pair', 'UPDATE soft_pairs SET tenant_id = 1 + tenant_id % 10 WHERE deleted_at IS NOT NULL AND dim_a_id < 40'),
	(9, 'buffered events', 'INSERT INTO events SELECT 6000 + i, CASE i % 4 WHEN 0 THEN 5 WHEN 1 THEN 20 WHEN 2 THEN 33 ELSE 100 + i % 50 END, timestamptz ''2026-02-05'', 4, ''u'' FROM generate_series(1, 120) i'),
	(10, 'promoted', 'SELECT sift.col_promote(''events_col'')'),
	(11, 'merged', 'SELECT sift.col_merge(''events_col'', true)'),
	(12, 'rebuilt', 'REINDEX INDEX events_col');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), stmt, 'SELECT * FROM probe(''events'', ''(SELECT * FROM soft_pairs WHERE deleted_at IS NULL)'')']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
SELECT sift.col_verify('events_col', true);

DROP FUNCTION probe(text, text);
DROP TABLE shapes, states;
DROP FUNCTION ms_run(text);
DROP TABLE events;
DROP TABLE soft_pairs;
DROP EXTENSION pg_sift;
