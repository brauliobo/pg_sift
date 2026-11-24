-- A writer of a UNIQUE sift_col index finds the keys of the parts earlier writers left without reading every part, and the check of ON CONFLICT does not write the stage of its transaction
-- for each row it checks: what the stage holds is found in the registry of the keys, what the partitions hold in the parts that can have the key.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.test_create_format1 = off;
\set VERBOSITY terse

-- several committed writers: a statement of more than 4096 rows is a part of its own
CREATE TABLE st_t (id int, v int DEFAULT 1);
CREATE UNIQUE INDEX st_i ON st_t USING sift_col (id);
INSERT INTO st_t SELECT g FROM generate_series(1, 5000) g;
INSERT INTO st_t SELECT g FROM generate_series(5001, 10000) g;
INSERT INTO st_t SELECT g FROM generate_series(10001, 15000) g;
INSERT INTO st_t SELECT g FROM generate_series(15001, 20000) g;
INSERT INTO st_t SELECT g FROM generate_series(20001, 25000) g;
SELECT count(*) AS parts_per_writer FROM sift.parts('st_i') WHERE kind = 'immutable';

-- a key of the first part, of the last, of none; the keys of a statement that has some of them twice
INSERT INTO st_t VALUES (1);
INSERT INTO st_t VALUES (25000);
INSERT INTO st_t VALUES (25001);
INSERT INTO st_t VALUES (12500), (25002);
INSERT INTO st_t VALUES (25003), (25003);
SELECT count(*) AS rows, count(DISTINCT id) AS keys FROM st_t;

-- the check of ON CONFLICT of 3000 keys writes no stage: some are in the parts, some in the stage of the transaction, some new
BEGIN;
SELECT sift._test_stage_flushes() AS flushes_before \gset
INSERT INTO st_t SELECT g FROM generate_series(24001, 27000) g ON CONFLICT (id) DO NOTHING;
INSERT INTO st_t SELECT g FROM generate_series(1, 27000) g WHERE g % 9 = 0 ON CONFLICT (id) DO UPDATE SET v = st_t.v + 1;
SELECT sift._test_stage_flushes() - :flushes_before AS stage_writes;
COMMIT;
SELECT count(*) AS rows, count(DISTINCT id) AS keys, sum(v) AS total FROM st_t;
SELECT v, count(*) FROM st_t GROUP BY v ORDER BY v;

-- the same keys after the commit: the parts hold them
INSERT INTO st_t VALUES (27000);
INSERT INTO st_t VALUES (27001);
SELECT count(*) AS rows FROM st_t;

DROP TABLE st_t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
