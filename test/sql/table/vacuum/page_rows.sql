-- VACUUM of a sift_table reads the rows of a write buffer page by the slots of its header, 291 at most. A page whose row count is over that is damaged: VACUUM refuses
-- it where it read a record from beyond the page and set a flag in it.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the row count of a write buffer page is Format 1
SET sift.test_create_format1 = on;
CREATE TABLE pr (id int) USING sift_table;
INSERT INTO pr SELECT generate_series(1, 20);
DELETE FROM pr WHERE id % 2 = 0;
SELECT count(*) AS live FROM pr;
VACUUM pr;

-- the row count of the first write buffer data page is 292, then 60,000 (SiftTblPageHdr.nrows: 4 bytes into the page data)
SELECT b AS blk FROM generate_series(0, pg_relation_size('pr') / 8192 - 1) b, LATERAL sift._test_page('pr', b) p WHERE p.type = 7 ORDER BY b LIMIT 1 \gset
SELECT sift._test_poke('pr', :blk, 24 + 4, 292, 2);
VACUUM pr;
SELECT sift._test_poke('pr', :blk, 24 + 4, 60000, 2);
VACUUM pr;
SELECT sift._test_poke('pr', :blk, 24 + 4, 10, 2);
SELECT count(*) AS live FROM pr;
DROP TABLE pr;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
