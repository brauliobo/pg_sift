-- A write buffer that holds no batch (a transaction that inserted and rolled back, the only row of the buffer) has no batch array: the aggregate scan asks the buffer
-- for its batches all the same, and a copy of zero bytes from a NULL pointer is undefined (UBSan halts the backend and the server restarts: the sanitizer build of the full
-- tier fails on it, and so does every job that runs in the cluster while it recovers).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.enable_agg = on;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;

CREATE TABLE we (id int PRIMARY KEY, g int, v int) WITH (autovacuum_enabled = off);
INSERT INTO we SELECT i, i % 5, i FROM generate_series(1, 20000) i;
CREATE INDEX we_col ON we USING sift_col (id, g, v);
BEGIN;
INSERT INTO we VALUES (500000, 1, 1);
ROLLBACK;

EXPLAIN (COSTS OFF) SELECT g, count(*) FROM we WHERE g < 3 GROUP BY g;
SELECT g, count(*), sum(v) FROM we WHERE g < 3 GROUP BY g ORDER BY g;
SELECT count(*), sum(v) FROM we WHERE g = 4;

DROP TABLE we;
DROP EXTENSION pg_sift;
