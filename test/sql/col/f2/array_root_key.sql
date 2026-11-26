-- A document that is an array or a string has no member t, and a path that declares t as an array of elements does not make it have one: jsonb_exists of '[1, 2]' and '"s"' is false
-- through SiftAggV6 and the bitmap scan, as the heap says, while the array that holds the string t and the string t itself (the key as a root) stay true.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE ark (id int NOT NULL, doc jsonb);
INSERT INTO ark SELECT x, CASE x % 8 WHEN 0 THEN NULL WHEN 1 THEN '[1, 2]' WHEN 2 THEN '"s"' WHEN 3 THEN 'null' WHEN 4 THEN '{"b": "x"}' WHEN 5 THEN '{"t": ["u"]}' WHEN 6 THEN '["t"]' ELSE '"t"' END::jsonb
FROM generate_series(1, 800) x;
CREATE TABLE arh AS SELECT * FROM ark;
CREATE INDEX ark_i ON ark USING sift_col (id, doc) WITH (paths = 'doc.b astext, doc.t[] text(8)');
VACUUM (FREEZE, ANALYZE) ark;

SELECT id % 8 AS root, count(*) FROM ark WHERE doc ? 't' GROUP BY 1 ORDER BY 1;
SELECT id % 8 AS root, count(*) FROM arh WHERE doc ? 't' GROUP BY 1 ORDER BY 1;
SELECT sift_helpers.agg_check_outcome('SELECT id % 8 AS root, count(*) FROM ark WHERE doc ? ''t'' GROUP BY 1') AS agg_outcome;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT sift_helpers.f2_same('SELECT id FROM ark WHERE doc ? ''t''', false) AS bitmap_outcome;
DROP TABLE ark, arh;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
