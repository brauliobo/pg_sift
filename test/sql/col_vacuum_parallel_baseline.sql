-- A VACUUM that vacuums the b-trees of a table with parallel workers runs the cleanup of a sift_col index in the leader, in parallel mode, where no transaction id may be assigned:
-- the repair of a log that a concurrent rebuild left incomplete (the baseline) waits for a VACUUM without workers instead of failing with "cannot assign transaction IDs during
-- a parallel operation"; until then the reads check the heap.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.col_auto_merge = off;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE pb (id int PRIMARY KEY, a int, b int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO pb SELECT i, i % 100, i % 7, repeat('p', 100) FROM generate_series(1, 30000) i;
CREATE INDEX pb_a ON pb (a);
CREATE INDEX pb_b ON pb (b);
CREATE INDEX pb_idx ON pb USING sift_col (id, a, b);
REINDEX INDEX CONCURRENTLY pb_idx;
SELECT complete FROM sift.col_tracking('pb_idx');
SET min_parallel_index_scan_size = 0;
SET max_parallel_maintenance_workers = 2;
VACUUM (PARALLEL 2) pb;
SELECT complete FROM sift.col_tracking('pb_idx');
SELECT count(*) = 30000 AS rows_read FROM pb WHERE a >= 0;
VACUUM (PARALLEL 0) pb;
SELECT complete FROM sift.col_tracking('pb_idx');
DROP TABLE pb;
DROP EXTENSION pg_sift CASCADE;
