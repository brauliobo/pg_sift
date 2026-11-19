-- A scan that paid for marks of the tombstone log asks whether the fold of the marks is due, which needs the marks a fold could clear: counting them is a pass over all the entries of the log
-- (24 instructions each: 1.08 million of the 5.0 million instructions of a scan of a log of 45,000 entries). The reads have to pay for the fold before it is due, and what they have paid is
-- a fraction of the cost of a fold of that many marks (1,100 instructions each), so the log is counted once and again only when the reads have paid half of the fold of what it counted: the
-- count of the last pass is a floor, since entries are added and only become clearable. Once the fold is due it stays so for the log, and no read counts it again. The mutants count at every scan
-- (80 times) and, when the fold is due, at every scan that comes after.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
CREATE TABLE r (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO r SELECT i, i % 10, i FROM generate_series(1, 30000) i;
CREATE INDEX r_c ON r USING sift_col (id, g, v) WITH (track_changes = 'id');
UPDATE r SET v = v + 1;
UPDATE r SET v = v + 1 WHERE id % 2 = 0;
\c -
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;
SELECT sift._test_col_fold_counts() AS counted_before;
SELECT count(*) AS rows_read, sum(v) AS sum_read FROM r WHERE id < 20000 AND g IN (3, 4) \gset
\set ECHO none
\o /dev/null
SELECT 'SELECT count(*) FROM r WHERE id < 20000 AND g IN (3, 4)' FROM generate_series(1, 80) \gexec
\o
\set ECHO all
-- the reads paid for the scans of a log of 45,000 entries past the cost of a fold of one mark (the first count) and half the cost of a fold of what it counted (the second, which finds the fold due)
SELECT sift._test_col_marks_paid('r_c') > 40000000 AS reads_paid_half_a_fold, sift._test_col_fold_counts() BETWEEN 2 AND 3 AS log_counted_twice, sift._test_col_fold_due('r_c') AS fold_due;
SELECT count(*) = :rows_read AND sum(v) = :sum_read AS same_rows FROM r WHERE id < 20000 AND g IN (3, 4);
DROP TABLE r;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
