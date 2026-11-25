-- A sift_col index keeps the values that its leaf cannot hold (a text of 100 MB) in the granules of its parts: a part of more than 1 GB of them is written, read and checked like any other
-- (the writer of a part never held it in one palloc), and rows of that width are cut into granules by their bytes, not only by their count.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;

-- one value of 100 MB, by an insert and by a build
CREATE TABLE wide1 (id int, v text);
CREATE INDEX wide1_idx ON wide1 USING sift_col (id, v);
INSERT INTO wide1 SELECT 1, repeat('a', 100 * 1024 * 1024);
SELECT id, length(v) FROM wide1;
SELECT sift.col_maintain('wide1_idx');
SELECT id, length(v), count(*) OVER () AS rows FROM wide1;
SELECT count(*) AS problems FROM sift.check('wide1_idx');
DROP TABLE wide1;

-- eleven of them in one part: the 1.15 GB of their granules are more than a StringInfo holds
CREATE TABLE wide11 (id int, v text);
INSERT INTO wide11 SELECT i, repeat(md5(i::text), 3276800) FROM generate_series(1, 11) i;
SET maintenance_work_mem = '2GB';
CREATE INDEX wide11_idx ON wide11 USING sift_col (id, v);
RESET maintenance_work_mem;
SELECT count(*) AS problems FROM sift.check('wide11_idx');
SELECT count(*) AS rows, sum(length(v)) AS bytes, bool_and(v = repeat(md5(id::text), 3276800)) AS same FROM wide11;
DROP TABLE wide11;

DROP EXTENSION pg_sift;
