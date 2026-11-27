-- CREATE UNIQUE INDEX on a sift index keeps the keys of its rows in a sort that spills past its share of the maintenance memory: the build
-- holds about what a plain one does (a table of every key in memory grew with the rows, and the build left their entries behind it), and a
-- duplicate is found however far apart its rows are. sift.test_build_peak is the most memory (kB) the build allocated, sampled at each run.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '4MB';
SET sift.test_run_docs = 2000;

CREATE TABLE ub (id int, body text);
INSERT INTO ub SELECT i, 'doc ' || i FROM generate_series(1, 30000) i;
CREATE TABLE peaks (kind text, kb bigint);
SET sift.test_build_peak = 0;
CREATE INDEX ub_plain ON ub USING sift (body);
INSERT INTO peaks SELECT 'plain', pg_size_bytes(current_setting('sift.test_build_peak')) / 1024;
DROP INDEX ub_plain;
SET sift.test_build_peak = 0;
CREATE UNIQUE INDEX ub_body ON ub USING sift (body);
INSERT INTO peaks SELECT 'unique', pg_size_bytes(current_setting('sift.test_build_peak')) / 1024;
SELECT u.kb - p.kb <= 4096 / 8 + 256 AS unique_within_its_share FROM peaks u, peaks p WHERE u.kind = 'unique' AND p.kind = 'plain';
SELECT count(*) FROM ub WHERE body @@> 'doc AND 29999';
INSERT INTO ub VALUES (30001, 'doc 1');

-- the duplicate rows are the first and the last of the table, in different runs of the sort
DROP INDEX ub_body;
INSERT INTO ub VALUES (30001, 'doc 1');
CREATE UNIQUE INDEX ub_body ON ub USING sift (body);
DELETE FROM ub WHERE id = 30001;
CREATE UNIQUE INDEX ub_body ON ub USING sift (body);
SELECT indisunique, indisvalid FROM pg_index WHERE indexrelid = 'ub_body'::regclass;
DROP TABLE ub, peaks;
DROP EXTENSION pg_sift;
