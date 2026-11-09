-- A plan that scans a sift_col index under a Gather runs the scan in a worker, which has none of the rows its leader holds back (col/wbuf/pending.c: the rows a
-- transaction inserts after its first, and the tombstones it has not logged). A plan that was made before the rows were inserted (a prepared statement) did not
-- flush them either, and the worker's scan missed them.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE own (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO own SELECT i, i % 5, i FROM generate_series(1, 20000) i;
CREATE INDEX own_c ON own USING sift_col (g, id, v) WITH (track_changes = id);
VACUUM (FREEZE, ANALYZE) own;
SET sift.enable_agg = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET plan_cache_mode = force_generic_plan;
SET debug_parallel_query = on;
PREPARE by_g AS SELECT g, id, v FROM own WHERE g = 2;
EXPLAIN (COSTS OFF) EXECUTE by_g;
-- the rows the plan returns, counted in a function whose plan is kept
CREATE FUNCTION by_g() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM g, id, v FROM own WHERE g = 2;
	GET DIAGNOSTICS n = ROW_COUNT;
	RETURN n;
END $$;
SELECT by_g();

-- rows inserted after the first one of the transaction, the plan made before them
BEGIN;
INSERT INTO own VALUES (20001, 2, 20001);
INSERT INTO own SELECT i, 2, i FROM generate_series(20002, 20101) i;
SELECT by_g();
ROLLBACK;

-- the second transaction of the session
BEGIN;
INSERT INTO own VALUES (20001, 2, 20001);
INSERT INTO own SELECT i, 2, i FROM generate_series(20002, 20101) i;
SELECT by_g();
-- a subtransaction that is rolled back leaves its rows out
SAVEPOINT s;
INSERT INTO own SELECT i, 2, i FROM generate_series(20102, 20201) i;
SELECT by_g();
ROLLBACK TO s;
SELECT by_g();
ROLLBACK;

-- tombstones held back by the transaction
BEGIN;
DELETE FROM own WHERE id = 2;
DELETE FROM own WHERE id = 7;
DELETE FROM own WHERE id = 12;
SELECT by_g();
ROLLBACK;

-- COPY
COPY own FROM STDIN;
20001	2	20001
20002	2	20002
20003	2	20003
\.
SELECT by_g();
SELECT count(*) FROM own WHERE g = 2;

DROP FUNCTION by_g();
DROP TABLE own;
DROP EXTENSION pg_sift;
