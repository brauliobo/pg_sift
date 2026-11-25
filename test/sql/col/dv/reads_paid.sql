-- The units that deletion records end rows of cost the scans that read them what their fold would spare (col/v1/scan/dvec.c): the reads pay for the fold of the records as they do for marks,
-- and the fold is due when what they paid comes to half of what it costs (col/v1/maint/fold.c). The fold of records is priced by the rows they name, not by one mark for each record.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
CREATE TABLE r (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO r SELECT i, i % 10, i FROM generate_series(1, 30000) i;
CREATE INDEX r_c ON r USING sift_col (id, g, v) WITH (track_changes = 'id');
VACUUM (FREEZE) r;
DELETE FROM r WHERE id % 20 = 3;
\c -
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET sift.enable_agg = on;
SELECT (SELECT count(*) FROM sift._test_col_log_records('r_c')) > 0 AS records_written,
       (SELECT count(*) FROM sift._test_col_log_entries('r_c') WHERE kind = 1 AND flags & 32768 = 0) AS marks;
SELECT sift._test_col_marks_paid('r_c') AS paid_before_reads, sift._test_col_fold_due('r_c') AS due_before_reads;
SELECT count(*), sum(v) FROM r WHERE g = 3 \gset
SELECT sift._test_col_marks_paid('r_c') > 0 AS one_read_paid, sift._test_col_fold_due('r_c') AS due_after_one_read;
\set ECHO none
\o /dev/null
SELECT 'SELECT count(*), sum(v) FROM r WHERE g = 3' FROM generate_series(1, 300) \gexec
\o
\set ECHO all
SELECT sift._test_col_marks_paid('r_c') > 19000000 AS reads_paid_half_a_fold, sift._test_col_fold_due('r_c') AS due_after_reads;
SELECT sift._test_col_fold_marks('r_c') > 0 AS folded;
SELECT (SELECT count(*) FROM sift._test_col_log_records('r_c')) AS records_after_fold, sift._test_col_fold_due('r_c') AS due_after_fold;
SELECT count(*) = :count AND sum(v) = :sum AS same_rows FROM r WHERE g = 3;
DROP TABLE r;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
