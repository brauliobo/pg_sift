-- A pre-aggregate of a granule less its dead rows (the delta subtraction) stays exact up to 5% dead rows; past that, or when a dead row is the least or greatest of its group,
-- the granule is read instead. Compacting rewrites the part with its pre-aggregate.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 20000, 0, 1, 0, 1, 0, true) > 0 AS added;
SELECT min(part_gen) AS g FROM sift._test_f2_parts('t_idx', 1, NULL) \gset

-- 64 dead rows of granule 0, none the least or greatest of its group: subtracted, and equal to the cube of the live rows
SELECT sift._test_f2_delete('t_idx', 1, NULL, 1, 1, 64, 100);
SELECT granule, kind, ndead, unsettled FROM sift._test_f2_dv('t_idx', 1, NULL, :g);
SELECT sift._test_f2_preagg('t_idx', 1, NULL, :g, 0);

-- 364 dead rows in the same granule are past the limit of 5% of its rows (and of 64 rows)
SELECT sift._test_f2_delete('t_idx', 1, NULL, 1, 1000, 300, 2);
SELECT granule, kind, ndead FROM sift._test_f2_dv('t_idx', 1, NULL, :g);
SELECT sift._test_f2_preagg('t_idx', 1, NULL, :g, 0);

-- A part made by compaction carries its pre-aggregate
SELECT done, rows_in, rows_out, dead_dropped FROM sift._test_f2_compact('t_idx', 1, NULL, ARRAY[:g], true);
SELECT level, nrows, ndead FROM sift._test_f2_parts('t_idx', 1, NULL);
SELECT sift._test_f2_check('t_idx');

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
