-- A sift_table that a Format 1 release made and that never got a row has no block: Format 1 writes its first block with its first row, while Format 2 writes its metapage when it
-- makes the file. The empty file stays a Format 1 table (reads give no row, the first write makes it Format 1), whatever format new tables get.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.test_create_format1 = on;
CREATE TABLE ef1 (id int PRIMARY KEY, v text) USING sift_table;
SET sift.test_create_format1 = off;
SELECT pg_relation_size('ef1') AS bytes, major FROM sift.index_format('ef1');
SELECT * FROM ef1;
SELECT count(*) FROM ef1 WHERE id = 1;
INSERT INTO ef1 VALUES (1, 'a'), (2, 'b'), (3, 'c');
UPDATE ef1 SET v = 'B' WHERE id = 2;
DELETE FROM ef1 WHERE id = 3;
INSERT INTO ef1 VALUES (1, 'x') ON CONFLICT (id) DO UPDATE SET v = excluded.v;
SELECT * FROM ef1 ORDER BY id;
SELECT major FROM sift.index_format('ef1');
SELECT count(*) AS problems FROM sift.check('ef1');
-- a new table of the session gets its metapage with its file, so it is never empty
CREATE TABLE ef2 (id int) USING sift_table;
SELECT pg_relation_size('ef2') > 0 AS has_metapage, major FROM sift.index_format('ef2');
RESET sift.test_create_format1;
DROP TABLE ef1, ef2;
DROP EXTENSION pg_sift_test, pg_sift;
