-- A plan counts the pre-aggregate sections of the units of an index (reading the cubes of a sample of the units of every segment) only to price an aggregate that
-- a pre-aggregate answers: a statement that does not (a top-N walk, a point lookup) reads no census, a priced one reads it once for each state of the index, and
-- a backend that connects later takes it from the shared ring. sift._test_col_pre_folds says how many the backend folded and how many it took.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET cpu_tuple_cost = 0.5;
SET sift.enable_agg = on;
CREATE TABLE pl (id int, cat int, ts timestamp, amt int) WITH (autovacuum_enabled = off);
INSERT INTO pl SELECT g, g % 7, timestamp '2026-01-01' + g * interval '1 minute', g % 100 FROM generate_series(1, 30000) g;
CREATE INDEX pl_idx ON pl USING sift_col (id, cat, ts, amt) WITH (order_columns = 'ts', order_array_columns = 'ts');
VACUUM (FREEZE, ANALYZE) pl;
\connect
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET sift.enable_agg = on;

-- plans that price no pre-aggregate answer
EXPLAIN (COSTS OFF) SELECT id FROM pl ORDER BY ts LIMIT 5;
EXPLAIN (COSTS OFF) SELECT id FROM pl WHERE id = 5;
SELECT built, shared FROM sift._test_col_pre_folds();

-- an aggregate that a pre-aggregate answers: counted once, the same plan again counts nothing
EXPLAIN (COSTS OFF) SELECT cat, count(*), sum(amt) FROM pl GROUP BY cat;
EXPLAIN (COSTS OFF) SELECT cat, count(*), sum(amt) FROM pl GROUP BY cat;
SELECT built, shared FROM sift._test_col_pre_folds();

-- another backend takes the count of the first
\connect
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET sift.enable_agg = on;
EXPLAIN (COSTS OFF) SELECT cat, count(*), sum(amt) FROM pl GROUP BY cat;
SELECT built, shared FROM sift._test_col_pre_folds();

-- a new segment is a new state of the index: counted again
INSERT INTO pl SELECT g, g % 7, timestamp '2026-01-01' + g * interval '1 minute', g % 100 FROM generate_series(30001, 40000) g;
SELECT sift.col_promote('pl_idx') > 0 AS promoted;
\connect
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET sift.enable_agg = on;
EXPLAIN (COSTS OFF) SELECT id FROM pl ORDER BY ts LIMIT 5;
SELECT built, shared FROM sift._test_col_pre_folds();
EXPLAIN (COSTS OFF) SELECT cat, count(*), sum(amt) FROM pl GROUP BY cat;
SELECT built, shared FROM sift._test_col_pre_folds();

DROP TABLE pl;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
