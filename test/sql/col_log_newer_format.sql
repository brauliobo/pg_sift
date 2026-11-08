-- A build opens the tombstone log of an index only when it reads the column format of the log: a log of a newer format may hold records an older build would not keep when it
-- appended, so the older build stops with an error and a hint instead (sift.test_col_log_max_read stands in for the older build).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i FROM generate_series(1, 1000) i;
CREATE INDEX t_c ON t USING sift_col (id, v);
UPDATE t SET v = v + 1 WHERE id = 1;
SELECT count(*) AS entries FROM sift._test_col_log_entries('t_c');

\c -
SET sift.test_col_log_max_read = 42;
UPDATE t SET v = v + 1 WHERE id = 2;
DELETE FROM t WHERE id = 3;
SELECT count(*) FROM sift._test_col_log_entries('t_c');

\c -
SET sift.test_col_log_max_read = 43;
UPDATE t SET v = v + 1 WHERE id = 2;
SELECT count(*) AS entries FROM sift._test_col_log_entries('t_c');
DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
