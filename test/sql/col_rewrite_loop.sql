-- The automatic rewrite of a connected index asks for the rewrite of the whole index only for what its readers cannot patch, and not again for what it did
-- (docs/COL-CONNECT.md): a parent whose other columns change, whose index is maintained and whose rows come in does not age the child.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, name text, updated_at timestamptz, score int) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100 FROM generate_series(1, 20000) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, name, score) WITH (track_changes = 'id');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, amount int, body text) WITH (autovacuum_enabled = false);
-- one row in ten points at a parent row the table does not have yet
INSERT INTO facts SELECT i, 1 + (i * 7919) % 22000, i % 5, 'c' || i FROM generate_series(1, 40000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, amount) WITH (track_changes = 'id', connect = 'dim_a_id -> dim_a(id)');
VACUUM dim_a, facts;
SELECT sift.col_maintain('facts_col') AS filled;
SELECT sift.col_maintenance_due('facts_col', true) AS due;

-- six rounds of the application: a few of the parent rows change a column the index does not key, children come in and change, the tables are vacuumed and the index of the parent maintained
CREATE TABLE trail (round int, due text);
\o /dev/null
SELECT s FROM generate_series(1, 6) r, LATERAL unnest(ARRAY[
	format('UPDATE dim_a SET updated_at = now() WHERE (id + %s * 7) %% 100 < 3', r),
	format('INSERT INTO facts SELECT 100000 + %s * 1000 + i, 1 + (i * 13) %% 20000, i %% 5, ''n'' FROM generate_series(1, 200) i', r),
	format('UPDATE facts SET body = body || ''x'' WHERE id BETWEEN %s * 100 AND %s * 100 + 199', r, r),
	'VACUUM dim_a, facts',
	'SELECT sift.col_maintain(''dim_a_col'')',
	format('INSERT INTO trail SELECT %s, sift.col_maintenance_due(''facts_col'', true)', r)]) WITH ORDINALITY u(s, o) ORDER BY r, o \gexec
\o
SELECT count(*) AS rounds, count(*) FILTER (WHERE due = 'stale connection') AS rounds_stale FROM trail;

DROP TABLE trail, facts, dim_a CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
