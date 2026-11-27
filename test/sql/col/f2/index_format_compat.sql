-- sift.index_format() and the segment listings answer for an index or a table of either format: the format is read from the relation (an empty one reports the format a new one would have),
-- and sift.table_segments() lists a sift_table of format 2.0 as it does one of format 1.
CREATE EXTENSION pg_sift_test CASCADE;
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE TABLE d (id int, body text);
INSERT INTO d VALUES (1, 'hello world');
CREATE INDEX dbm ON d USING sift (body);
SELECT 'bm25' AS what, major, minor, long_words IS NOT NULL AS has_long_words FROM sift.index_format('dbm');
SELECT 'heap' AS what, count(*) FROM sift.index_format('d');
-- format 1
SET sift.test_create_format1 = on;
CREATE TABLE t1 (id int PRIMARY KEY, v int, s text) USING sift_table;
SELECT 'f1 empty sift_table' AS what, major, minor FROM sift.index_format('t1');
INSERT INTO t1 SELECT g, g, 'x' || g FROM generate_series(1, 1000) g;
SELECT 'f1 sift_table' AS what, major, minor FROM sift.index_format('t1');
SELECT 'f1 table_segments' AS what, count(*) > 0 AS listed FROM sift.table_segments('t1');
CREATE TABLE h1 (id int, g int, v int);
INSERT INTO h1 SELECT g, g % 10, g FROM generate_series(1, 1000) g;
CREATE INDEX hc1 ON h1 USING sift_col (id, g, v);
SELECT 'f1 sift_col' AS what, major, minor FROM sift.index_format('hc1');
SELECT 'f1 col_segments' AS what, count(*) > 0 AS listed FROM sift.col_segments('hc1');
-- format 2.0
SET sift.test_create_format1 = off;
CREATE TABLE t2 (id int PRIMARY KEY, v int, s text) USING sift_table;
SELECT 'f2 empty sift_table' AS what, major, minor FROM sift.index_format('t2');
INSERT INTO t2 SELECT g, g, 'x' || g FROM generate_series(1, 1000) g;
SELECT 'f2 sift_table' AS what, major, minor FROM sift.index_format('t2');
SELECT 'f2 table_segments' AS what, count(*) > 0 AS listed FROM sift.table_segments('t2');
CREATE TABLE h2 (id int, g int, v int);
INSERT INTO h2 SELECT g, g % 10, g FROM generate_series(1, 1000) g;
CREATE INDEX hc2 ON h2 USING sift_col (id, g, v);
SELECT 'f2 sift_col' AS what, major, minor FROM sift.index_format('hc2');
SELECT 'f2 col_segments' AS what, count(*) > 0 AS listed FROM sift.col_segments('hc2');
-- a hook that reads the storage of Format 1 refuses an index of Format 2.0, which stays in service (a write is indexed, a read answers)
SELECT sift._test_col_fold_marks('hc2');
INSERT INTO h2 VALUES (1001, 1, 1001);
SELECT 'f2 sift_col after a hook of Format 1' AS what, count(*), sum(v) FROM h2 WHERE g = 1;
RESET ALL;
DROP TABLE d, t1, h1, t2, h2 CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
