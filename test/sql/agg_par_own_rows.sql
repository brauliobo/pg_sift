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

-- the plan of a child that a worker starts per combination reads an index of its own table, and the rows the transaction holds back for it are not in a page either
CREATE TABLE zones (id int, name text) WITH (autovacuum_enabled = false);
INSERT INTO zones SELECT i, 'z' || i % 7 FROM generate_series(0, 59) i;
CREATE INDEX zones_c ON zones USING sift_col (id, name);
CREATE TABLE zev (id int PRIMARY KEY, zone_id int) WITH (autovacuum_enabled = false);
INSERT INTO zev SELECT i, i % 65 FROM generate_series(1, 12000) i;
CREATE INDEX zev_c ON zev USING sift_col (id, zone_id);
VACUUM (FREEZE, ANALYZE) zones, zev;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT s.name, count(*) FROM zev e JOIN LATERAL (SELECT name FROM zones z WHERE z.id = e.zone_id LIMIT 1) s ON true GROUP BY 1;
BEGIN;
INSERT INTO zones VALUES (60, 'new');
INSERT INTO zones SELECT i, 'new' FROM generate_series(61, 64) i;
SELECT s.name, count(*) FROM zev e JOIN LATERAL (SELECT name FROM zones z WHERE z.id = e.zone_id LIMIT 1) s ON true GROUP BY 1 ORDER BY 1;
ROLLBACK;
BEGIN;
INSERT INTO zones VALUES (60, 'new');
INSERT INTO zones SELECT i, 'new' FROM generate_series(61, 64) i;
SELECT s.name, count(*) FROM zev e JOIN LATERAL (SELECT name FROM zones z WHERE z.id = e.zone_id LIMIT 1) s ON true GROUP BY 1 ORDER BY 1;
ROLLBACK;
DROP TABLE zones, zev;

-- the rows held back for the index of a partition
SET sift.agg_preagg = off;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE TABLE pt (id int, g int NOT NULL, v int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10000) WITH (fillfactor = 10, autovacuum_enabled = false);
CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10000) TO (20000) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO pt SELECT i, i % 5, i FROM generate_series(1, 19999) i;
CREATE INDEX pt_c ON pt USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) pt;
EXPLAIN (COSTS OFF) SELECT g, count(*), sum(v) FROM pt GROUP BY g;
BEGIN;
INSERT INTO pt VALUES (9000, 1, 9000);
INSERT INTO pt SELECT 10000 + i, 1, i FROM generate_series(1, 100) i;
INSERT INTO pt SELECT 100 + i, 1, i FROM generate_series(1, 100) i;
SELECT g, count(*), sum(v) FROM pt GROUP BY g ORDER BY g;
ROLLBACK;
DROP TABLE pt;

DROP TABLE own;
DROP EXTENSION pg_sift;
