-- The leaves a connected index takes from its parent (docs/COL-CONNECT.md): a flag that the parent row was found and the leaves of each carried column, after the leaves of the
-- columns of the table. Their segments are column format minor 45, and everything a sift_col index does (promotion, merge, VACUUM, verify) works with them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text, price numeric(30,4), born date, seen timestamptz, js jsonb, tags int[]) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 10, 'l' || i, i * 1.25, date '2026-01-01' + i, timestamptz '2026-01-01' + i * interval '1 hour', '{}', '{1}' FROM generate_series(1, 500) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label, price, born, seen, js, tags) WITH (paths = 'js.k int4');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 500, i FROM generate_series(1, 3000) i;

-- columns that cannot be carried
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id) carries js');
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id) carries tags');

CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, value_num) WITH (connect = 'dim_a_id -> dim_a(id) carries category_id, label, price, born, seen');
SELECT leaf, path, type FROM sift.col_leaves('events_col') ORDER BY leaf;
SELECT DISTINCT format FROM sift.col_segments('events_col') WHERE kind = 'immutable';
SELECT sift.col_verify('events_col', true);

-- the write buffer, promotion, merge, VACUUM and verify with the leaves
INSERT INTO events SELECT i, 1 + i % 500, i FROM generate_series(3001, 4000) i;
UPDATE events SET value_num = value_num + 1 WHERE id % 7 = 0;
DELETE FROM events WHERE id % 11 = 0;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
VACUUM events;
SELECT sift.col_verify('events_col', true);
SET sift.enable_agg = on;
SELECT count(*), sum(value_num), count(DISTINCT dim_a_id) FROM events;
SET sift.enable_agg = off;
SELECT count(*), sum(value_num), count(DISTINCT dim_a_id) FROM events;
RESET sift.enable_agg;
SELECT DISTINCT format FROM sift.col_segments('events_col') WHERE kind = 'immutable';

-- REINDEX builds the same leaves; connect = 'off' and a rebuild drop them
REINDEX INDEX events_col;
SELECT count(*) AS leaves FROM sift.col_leaves('events_col');
ALTER INDEX events_col SET (connect = 'off');
REINDEX INDEX events_col;
SELECT count(*) AS leaves FROM sift.col_leaves('events_col');
SELECT DISTINCT format FROM sift.col_segments('events_col') WHERE kind = 'immutable';

-- more leaves than an index holds
SET sift.test_limits = 'col_leaves=6';
CREATE INDEX bad ON events USING sift_col (id, dim_a_id) WITH (connect = 'dim_a_id -> dim_a(id) carries category_id, label, price, born, seen');
RESET sift.test_limits;

-- several connections in one index: two parents, each with its own leaves, dependency, stamp and fallback
CREATE TABLE dim_b (id int PRIMARY KEY, zone text NOT NULL, score int) WITH (autovacuum_enabled = false);
INSERT INTO dim_b SELECT i, 'r' || i % 5, i % 7 FROM generate_series(1, 200) i;
CREATE INDEX dim_b_col ON dim_b USING sift_col (id, zone, score);
CREATE TABLE users (id int PRIMARY KEY, name text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO users SELECT i, 'u' || i FROM generate_series(1, 50) i;
CREATE INDEX users_col ON users USING sift_col (id, name);
CREATE TABLE facts (id int PRIMARY KEY, a_id int NOT NULL, b_id int NOT NULL, created_by_id int NOT NULL, updated_by_id int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + i % 500, 1 + i % 200, 1 + i % 50, 1 + i % 49, i FROM generate_series(1, 3000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, a_id, b_id, created_by_id, updated_by_id, v)
  WITH (connect = 'a_id -> dim_a(id) carries category_id; b_id -> dim_b(id) carries zone, score via map; created_by_id -> users(id) carries name; updated_by_id -> users(id) carries name');
SELECT leaf, path, type FROM sift.col_leaves('facts_col') WHERE leaf >= 6 ORDER BY leaf;
SELECT ordinal, fk, parent, declaration, via FROM sift.col_connections WHERE child = 'facts_col'::regclass ORDER BY ordinal;
SELECT count(*) AS edges, count(DISTINCT refobjid) AS parents FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'facts_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n';
SELECT DISTINCT format FROM sift.col_segments('facts_col') WHERE kind = 'immutable';
SELECT sift.col_verify('facts_col', true);
REINDEX INDEX facts_col;
SELECT count(*) AS leaves FROM sift.col_leaves('facts_col');

-- one parent that does not vouch leaves the others as they are
SELECT sift._test_log_chan('dim_b_col', 0) IS NOT NULL AS dim_b_log_cleared;
SELECT parent, stamp FROM sift.col_connections WHERE child = 'facts_col'::regclass ORDER BY ordinal;
SELECT sift._test_log_chan('dim_b_col', 3) IS NOT NULL AS restored;

-- refused: a column connected twice, a column that is not in the index, too many connections, too many bytes
CREATE INDEX bad ON facts USING sift_col (id, a_id, b_id) WITH (connect = 'a_id -> dim_a(id) carries category_id; a_id -> dim_b(id) carries zone');
CREATE INDEX bad ON facts USING sift_col (id, a_id, b_id) WITH (connect = 'a_id -> dim_a(id) carries category_id; updated_by_id -> users(id) carries name');
CREATE INDEX bad ON facts USING sift_col (id, a_id, b_id) WITH (connect = 'a_id -> dim_a(id) carries category_id;');
SELECT sift_cols AS nine FROM (SELECT string_agg('c' || i || ' -> dim_a(id) carries category_id', '; ') AS sift_cols FROM generate_series(1, 9) i) x \gset
CREATE INDEX bad ON facts USING sift_col (id, a_id, b_id) WITH (connect = :'nine');
CREATE TABLE wide_dim (id int PRIMARY KEY, c1 interval, c2 interval, c3 interval, c4 interval, c5 interval, c6 interval, c7 interval, c8 interval, c9 interval, c10 interval, c11 interval,
  c12 interval, c13 interval, c14 interval, c15 interval, c16 interval, c17 interval, c18 interval, c19 interval, c20 interval, c21 interval, c22 interval, c23 interval) WITH (autovacuum_enabled = false);
CREATE INDEX wide_dim_col ON wide_dim USING sift_col (id, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21, c22, c23);
CREATE INDEX bad ON facts USING sift_col (id, a_id, b_id) WITH (connect = 'a_id -> wide_dim(id) carries c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21, c22, c23');
DROP TABLE wide_dim;

-- the parent index under a child index: refused; with CASCADE the whole child index goes (it would be wrong with one connection missing)
DROP INDEX users_col;
DROP INDEX users_col CASCADE;
SELECT count(*) AS child_indexes FROM pg_class WHERE relname = 'facts_col';
DROP TABLE facts, users, dim_b;
DROP TABLE events, dim_a CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
