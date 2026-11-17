-- The reader of the write buffer holds the records of the open segment once, at their size: 94,000 rows of a segment whose seal lags far behind its limit
-- (the crash test of the buffer, col_wbuf_crash) read within the col_write_buffer budget of test/mem-budgets.tsv, which the test build asserts on this call.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE t (doc int);
CREATE INDEX t_idx ON t USING sift_col (doc) WITH (write_buffer_limit = 512);
SELECT sift._test_col_wbuf_prepare('t_idx') IS NOT NULL AS prepared;
SELECT count(sift._test_col_wbuf_append('t_idx', 7, g * 40, 40)) AS calls FROM generate_series(0, 2349) g;
SELECT count(*) AS segments, sum(nrows) AS rows FROM sift._test_col_wbuf_segments('t_idx');
SELECT count(*) AS rows, count(DISTINCT id) AS ids FROM sift._test_col_wbuf_batches('t_idx', 7);
DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
