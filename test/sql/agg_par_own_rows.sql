-- The workers of a parallel aggregate read the pages of the write buffer of a sift_col index; the rows a transaction has inserted after its first one are held back
-- in the backend until it reads the index (col/wbuf/pending.c), and a worker has none of its own to flush. With the leader not scanning, nothing flushed them: the
-- transaction's own rows were missing from the answer of the workers, from the second transaction of a session on (the first read of a session flushed them).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE own (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO own SELECT i, i % 5, i FROM generate_series(1, 20000) i;
CREATE INDEX own_c ON own USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) own;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SET parallel_leader_participation = off;

-- the same aggregate, in the transaction that inserted rows, under the workers and under the heap's plan (sift.enable_agg off); three transactions, each rolled back
BEGIN;
INSERT INTO own SELECT i, i % 5, i FROM generate_series(20001, 20100) i;
SELECT count(*), sum(v) FROM own;
SELECT g, count(*), sum(v) FROM own GROUP BY g ORDER BY g;
ROLLBACK;
BEGIN;
INSERT INTO own SELECT i, i % 5, i FROM generate_series(20001, 20100) i;
SELECT count(*), sum(v) FROM own;
SELECT g, count(*), sum(v) FROM own GROUP BY g ORDER BY g;
SET LOCAL sift.enable_agg = off;
SELECT count(*), sum(v) FROM own;
ROLLBACK;
BEGIN;
INSERT INTO own SELECT i, i % 5, i FROM generate_series(20001, 20100) i;
INSERT INTO own SELECT i, i % 5, i FROM generate_series(20101, 20200) i;
SELECT count(*), sum(v) FROM own;
ROLLBACK;
SELECT count(*), sum(v) FROM own;

DROP TABLE own;
DROP EXTENSION pg_sift;
