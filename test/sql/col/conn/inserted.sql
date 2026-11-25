-- sift_conn_inserted_since(stamp): could a row have been inserted into the parent index by a transaction at or after the stamp's horizon? False only when every segment says its
-- inserters finished before the horizon and the write buffer holds no live record from the horizon on (an aborted transaction does not count; the caller's own insert does).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 10, 'l' || i FROM generate_series(1, 1000) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label);
SELECT sift._test_conn_enable('dim_a_col') <> 0 AS journal;
CREATE FUNCTION ins(lin int8, hor xid) RETURNS bool LANGUAGE sql AS $$ SELECT sift._test_conn_inserted_since('dim_a_col', lin, hor) $$;

-- nothing written since the stamp
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_a_col') \gset
SELECT ins(:lin, :'hor') AS nothing_written;

-- a committed insert of a new key
INSERT INTO dim_a VALUES (2001, 1, 'new');
SELECT ins(:lin, :'hor') AS committed_insert;

-- a stamp taken after it: the insert is before the horizon, in the write buffer
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_a_col') \gset
SELECT ins(:lin, :'hor') AS after_the_insert;

-- a rolled back insert, and one rolled back to a savepoint
BEGIN;
INSERT INTO dim_a VALUES (2002, 1, 'gone');
ROLLBACK;
SELECT ins(:lin, :'hor') AS rolled_back;
BEGIN;
SAVEPOINT s;
INSERT INTO dim_a VALUES (2003, 1, 'gone');
ROLLBACK TO s;
SELECT ins(:lin, :'hor') AS rolled_back_savepoint;
COMMIT;
SELECT ins(:lin, :'hor') AS after_the_savepoint;

-- the transaction's own insert, before it commits, and one inside a savepoint that is released
BEGIN;
INSERT INTO dim_a VALUES (2004, 1, 'own');
SELECT ins(:lin, :'hor') AS own_insert;
ROLLBACK;
SELECT ins(:lin, :'hor') AS own_insert_rolled_back;
BEGIN;
SAVEPOINT s;
INSERT INTO dim_a VALUES (2005, 1, 'own');
RELEASE s;
SELECT ins(:lin, :'hor') AS savepoint_released;
ROLLBACK;

-- an UPDATE writes a new version of the row: it counts (its key is in changed(S, X) as well)
UPDATE dim_a SET category_id = 7 WHERE id = 5;
SELECT ins(:lin, :'hor') AS update;

-- promotion and VACUUM move the rows into segments, which say when their inserters finished: the old stamp still sees the insert, a new one does not
SELECT sift.col_promote('dim_a_col') >= 0 AS promoted;
VACUUM dim_a;
SELECT ins(:lin, :'hor') AS old_stamp_after_vacuum;
SELECT lineage AS lin2, horizon AS hor2 FROM sift._test_conn_take('dim_a_col') \gset
SELECT ins(:lin2, :'hor2') AS new_stamp_after_vacuum;
SELECT sift.col_merge('dim_a_col', true) >= 0 AS merged;
SELECT ins(:lin2, :'hor2') AS after_merge;
INSERT INTO dim_a VALUES (2006, 1, 'late');
SELECT ins(:lin2, :'hor2') AS after_merge_and_insert;

DROP FUNCTION ins(int8, xid);
DROP TABLE dim_a;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
