-- the VACUUM of a table whose sift_col index is damaged fails once with the error of the damage and takes the index out of service: the VACUUM that follows skips it (the warning
-- says so), the dead rows are removed from the heap, and the statements that follow give the answers of the heap
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 20000) g;
CREATE INDEX t_idx ON t USING sift_col (id, v);
DELETE FROM t WHERE id % 5 = 0;
SELECT count(*) AS rows_before, sum(id) FILTER (WHERE v = 3) AS ids_before FROM t;

-- the liveness run of the segment gets another page type
SELECT first_block AS live FROM sift.col_extents('t_idx') WHERE what = 'liveness run' \gset
SELECT sift._test_poke('t_idx', :live, 8176, 9, 2);
\set VERBOSITY terse
VACUUM t;
\set VERBOSITY default
SELECT index, error_code, detections, skipped_writes, skipped_vacuums, refused_scans FROM sift.quarantine;
SELECT n_dead_tup AS dead_after_vacuum FROM pg_stat_all_tables WHERE relid = 't'::regclass;
SELECT count(*) AS rows_after, sum(id) FILTER (WHERE v = 3) AS ids_after FROM t;

-- a second VACUUM has nothing to do with the index and says so
VACUUM t;
SELECT index, detections, skipped_vacuums FROM sift.quarantine;
VACUUM (ANALYZE) t;

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
VACUUM t;
SELECT count(*) AS rows_after_reindex FROM t;

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
