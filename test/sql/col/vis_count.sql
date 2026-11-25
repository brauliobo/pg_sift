-- The all-visible pages the planner and the aggregate scan count are those of the visibility map, for a heap of any number of pages (the map is read a word at
-- a time, with a partial word at the end of the last map page used). A page holds one row, VACUUM sets every bit and the DELETE clears those of the pages of the
-- rows it removes, so the share is known: (rows - deleted) / rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;

CREATE TABLE vc1 (id int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
CREATE TABLE vc7 (LIKE vc1) WITH (fillfactor = 10, autovacuum_enabled = false);
CREATE TABLE vc31 (LIKE vc1) WITH (fillfactor = 10, autovacuum_enabled = false);
CREATE TABLE vc33 (LIKE vc1) WITH (fillfactor = 10, autovacuum_enabled = false);
CREATE TABLE vc255 (LIKE vc1) WITH (fillfactor = 10, autovacuum_enabled = false);
CREATE TABLE vc4001 (LIKE vc1) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO vc1 SELECT i, repeat('x', 600) FROM generate_series(1, 1) i;
INSERT INTO vc7 SELECT i, repeat('x', 600) FROM generate_series(1, 7) i;
INSERT INTO vc31 SELECT i, repeat('x', 600) FROM generate_series(1, 31) i;
INSERT INTO vc33 SELECT i, repeat('x', 600) FROM generate_series(1, 33) i;
INSERT INTO vc255 SELECT i, repeat('x', 600) FROM generate_series(1, 255) i;
INSERT INTO vc4001 SELECT i, repeat('x', 600) FROM generate_series(1, 4001) i;
VACUUM vc1, vc7, vc31, vc33, vc255, vc4001;
DELETE FROM vc1 WHERE id % 5 = 1;
DELETE FROM vc7 WHERE id % 5 = 1;
DELETE FROM vc31 WHERE id % 5 = 1;
DELETE FROM vc33 WHERE id % 5 = 1;
DELETE FROM vc255 WHERE id % 5 = 1;
DELETE FROM vc4001 WHERE id % 5 = 1;
CREATE INDEX vc1_col ON vc1 USING sift_col (id);
CREATE INDEX vc7_col ON vc7 USING sift_col (id);
CREATE INDEX vc31_col ON vc31 USING sift_col (id);
CREATE INDEX vc33_col ON vc33 USING sift_col (id);
CREATE INDEX vc255_col ON vc255 USING sift_col (id);
CREATE INDEX vc4001_col ON vc4001 USING sift_col (id);

-- the pages of each heap, the rows left, and whether the share counted is the rows left over the pages
SELECT t, pg_relation_size(t) / 8192 AS pages,
	   abs((SELECT allvisible_frac FROM sift._test_col_leaf_stats(t || '_col') LIMIT 1) - (SELECT (n - (n + 4) / 5)::float8 / n FROM (SELECT right(t::text, -2)::int AS n) q)) < 1e-9 AS share_ok
FROM unnest(ARRAY['vc1', 'vc7', 'vc31', 'vc33', 'vc255', 'vc4001']::regclass[]) t ORDER BY 2;

DROP TABLE vc1, vc7, vc31, vc33, vc255, vc4001;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
