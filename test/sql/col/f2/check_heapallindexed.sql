-- sift.check(index, true) compares every row the heap shows with the index in sorts of maintenance_work_mem that spill, so that the size of the table is no limit of it: the two lists of
-- addresses of 150000 rows are more than the 1 MB it holds (a size over 50000 rows: the point is the size, one past the memory of a list in RAM)
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE chk (id bigint, g int);
CREATE INDEX chk_idx ON chk USING sift_col (id, g) WITH (key_order = 'id');
-- 150000 rows: the two lists of addresses (8 bytes each) pass the 1 MB the sorts hold, so that they spill
INSERT INTO chk SELECT i, i % 7 FROM generate_series(1, 150000) i;
SET maintenance_work_mem = '1MB';
SELECT count(*) FROM sift.check('chk_idx', true);
SELECT count(*) FROM sift.check('chk_idx', false);

DROP TABLE chk;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
