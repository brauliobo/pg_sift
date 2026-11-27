-- CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY of a sift_table validate its rows in windows of addresses that one allocation holds, whatever maintenance_work_mem is,
-- and no larger than the table: a budget past 2 GB builds the index as a smaller one does.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE TABLE facts (id int NOT NULL, value_num int) USING sift_table;
INSERT INTO facts SELECT g, g % 7 FROM generate_series(1, 300) g;
SET maintenance_work_mem = '3GB';
CREATE UNIQUE INDEX CONCURRENTLY facts_id ON facts (id);
REINDEX INDEX CONCURRENTLY facts_id;
CREATE INDEX CONCURRENTLY facts_value ON facts (value_num);
RESET maintenance_work_mem;
SELECT indexrelid::regclass AS index, indisvalid FROM pg_index WHERE indrelid = 'facts'::regclass ORDER BY indexrelid::regclass::text COLLATE "C";
SET enable_seqscan = off;
SELECT count(*) AS by_value FROM facts WHERE value_num = 3;
SELECT value_num FROM facts WHERE id = 250;
RESET enable_seqscan;
DROP TABLE facts;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
