-- What a Format 2.0 sift_table keeps of its relation (the store with the attributes of its indexes) and of its file (the manifest snapshot, the part pins, the address index, the point cache) follows
-- the relation: an index made or dropped on a table that has one already, and a column dropped, change no pg_class row, and a TRUNCATE of a table made in the transaction keeps its file and starts
-- its part generations again. Every row printed is what the statement returned.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET sift.enable_agg = off;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

-- 1. an index made on a table that has one: an UPDATE of the indexed column of a row of a part must reach the new index
CREATE TABLE inv (id int PRIMARY KEY, k int, a int, b int, v text) USING sift_table;
INSERT INTO inv SELECT i, 10 + i, i, i, 'a' FROM generate_series(1, 2000) i;
SELECT busy FROM sift.table_maintain('inv');
UPDATE inv SET v = 'warm' WHERE id = 2;
CREATE INDEX inv_k ON inv (k);
SELECT ctid AS before_ctid FROM inv WHERE id = 1 \gset
UPDATE inv SET k = 5000 WHERE id = 1;
SELECT ctid <> :'before_ctid' AS left_its_address FROM inv WHERE id = 1;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM inv WHERE k = 5000;
SELECT id FROM inv WHERE k = 5000;
SELECT count(*) FROM (SELECT id FROM inv WHERE k = 11 OFFSET 0) q;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*) FROM inv WHERE k + 0 = 5000;

-- 2. the index dropped: the column is free to be a column delta again
DROP INDEX inv_k;
SELECT busy FROM sift.table_maintain('inv');
SELECT ctid AS before_ctid FROM inv WHERE id = 3 \gset
UPDATE inv SET k = 7000 WHERE id = 3;
SELECT ctid = :'before_ctid' AS kept_its_address FROM inv WHERE id = 3;

-- 3. a unique index made later: a duplicate of a row of a part is refused
CREATE UNIQUE INDEX inv_a ON inv (a);
UPDATE inv SET a = 1 WHERE id = 5;
SELECT count(*) FROM inv WHERE a = 1;

-- 4. a column dropped: the rows keep the columns they have
ALTER TABLE inv DROP COLUMN b;
UPDATE inv SET v = 'dropped' WHERE id = 4;
SELECT id, k, a, v FROM inv WHERE id = 4;

-- 5. TRUNCATE of a table made in the transaction: the row before it is gone for every kind of read
BEGIN;
CREATE TABLE trunc (id int PRIMARY KEY, v text) USING sift_table;
INSERT INTO trunc VALUES (1, 'BEFORE_TRUNCATE');
SELECT v FROM trunc WHERE id = 1;
SET enable_seqscan = off;
SELECT v FROM trunc WHERE id = 1;
RESET enable_seqscan;
TRUNCATE trunc;
INSERT INTO trunc VALUES (1, 'AFTER_TRUNCATE');
SELECT v FROM trunc;
SET enable_seqscan = off;
SELECT v FROM trunc WHERE id = 1;
RESET enable_seqscan;
ROLLBACK;

-- 6. the same with rows in parts (a part is not rewritten by the TRUNCATE: its file is)
BEGIN;
CREATE TABLE trunc2 (id int PRIMARY KEY, v text) USING sift_table;
INSERT INTO trunc2 SELECT i, 'BEFORE' FROM generate_series(1, 3000) i;
SELECT busy FROM sift.table_maintain('trunc2');
SELECT count(*) FROM trunc2 WHERE v = 'BEFORE';
TRUNCATE trunc2;
INSERT INTO trunc2 SELECT i, 'AFTER' FROM generate_series(1, 3000) i;
SELECT busy FROM sift.table_maintain('trunc2');
SELECT v, count(*) FROM (SELECT v FROM trunc2 WHERE id + 0 > 0 OFFSET 0) q GROUP BY v;
SET enable_seqscan = off;
SELECT v FROM trunc2 WHERE id = 1500;
RESET enable_seqscan;
ROLLBACK;

DROP TABLE inv;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
