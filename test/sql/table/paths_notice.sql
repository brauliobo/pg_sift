-- sift.table_set_paths says when the column groups written before the declaration keep the column as one leaf (the paths are used once VACUUM FULL has
-- rewritten them); a table with no groups yet, and a declaration that is removed, say nothing.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE pn (id int, doc jsonb) USING sift_table;
INSERT INTO pn SELECT i, jsonb_build_object('k', i % 5) FROM generate_series(1, 2000) i;
SELECT sift.table_set_paths('pn', 'doc', 'doc.k float8');
SELECT sift.table_set_paths('pn', 'doc', NULL);
VACUUM (FREEZE) pn;
SELECT sift.table_set_paths('pn', 'doc', 'doc.k float8');
SELECT sift.table_set_paths('pn', 'doc', NULL);
VACUUM FULL pn;
SELECT sift.table_set_paths('pn', 'doc', 'doc.k float8');

DROP TABLE pn;
DROP EXTENSION pg_sift;
