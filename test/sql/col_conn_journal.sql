-- The key journal of a connected parent index (src/col/conn.c, docs/COL-CONNECT.md): once a child index is connected, the parent keeps a record (key, transaction) for every
-- entry an UPDATE, DELETE or key change ended, and sift_conn_changed(stamp) returns the keys of the records at or after the stamp's horizon, or says why it cannot.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 10, 'l' || i, 'x' FROM generate_series(1, 2000) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label);
-- an index nothing is connected to keeps no journal
SELECT why FROM sift._test_conn_take('dim_a_col');
SELECT count(*) AS journal_records FROM sift._test_col_log_entries('dim_a_col') WHERE kind = 4;
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 1;
SELECT count(*) AS journal_records FROM sift._test_col_log_entries('dim_a_col') WHERE kind = 4;

-- connecting makes the journal; again, the same one
SELECT sift._test_conn_enable('dim_a_col') <> 0 AS journal;
SELECT sift._test_conn_enable('dim_a_col') = (SELECT lineage FROM sift._test_conn_gen('dim_a_col')) AS same_lineage;
SELECT lineage = (SELECT lineage FROM sift._test_conn_gen('dim_a_col')) AS stamp_lineage, why FROM sift._test_conn_take('dim_a_col');
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_a_col') \gset

-- nothing changed since the stamp
SELECT why, key FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');

-- UPDATE, DELETE and a key change put their keys in the set; an INSERT of a new key does not (a key the derived value has none for is read by the lookup, never taken for absent)
UPDATE dim_a SET category_id = category_id + 100 WHERE id IN (10, 11, 12);
DELETE FROM dim_a WHERE id = 20;
UPDATE dim_a SET id = 5005 WHERE id = 30;
INSERT INTO dim_a VALUES (9999, 1, 'new', 'x');
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 10;
SELECT why, key FROM sift._test_conn_changed('dim_a_col', :lin, :'hor') ORDER BY key;
SELECT why, key FROM sift._test_conn_changed('dim_a_col', :lin, :'hor', 3) ORDER BY key;

-- a rolled back transaction and a rolled back savepoint leave nothing
BEGIN;
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 40;
ROLLBACK;
BEGIN;
SAVEPOINT s;
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 41;
ROLLBACK TO s;
COMMIT;
SELECT count(*) AS rolled_back_keys FROM sift._test_conn_changed('dim_a_col', :lin, :'hor') WHERE key IN (40, 41);

-- the transaction's own change is in the set before it commits
BEGIN;
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 50;
SELECT count(*) AS own_change FROM sift._test_conn_changed('dim_a_col', :lin, :'hor') WHERE key = 50;
ROLLBACK;

-- a later stamp sees only what came after it
SELECT lineage AS lin2, horizon AS hor2 FROM sift._test_conn_take('dim_a_col') \gset
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 60;
SELECT why, key FROM sift._test_conn_changed('dim_a_col', :lin2, :'hor2') ORDER BY key;

-- the records outlive a VACUUM (they are not frozen): the old stamp still sees every key
VACUUM dim_a;
SELECT count(*) AS keys_after_vacuum FROM sift._test_conn_changed('dim_a_col', :lin, :'hor') WHERE key IS NOT NULL;

-- a snapshot older than the horizon, a serializable one, another lineage, a stamp the journal was trimmed past
SELECT why FROM sift._test_conn_changed('dim_a_col', :lin, ((:'hor')::text::bigint + 100000)::text::xid);
SELECT why FROM sift._test_conn_changed('dim_a_col', :lin + 1, :'hor');
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT why FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');
ROLLBACK;
SELECT sift._test_log_chan('dim_a_col', 0) IS NOT NULL AS cleared;
SELECT why FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');
SELECT sift._test_log_chan('dim_a_col', 3) IS NOT NULL AS restored;
SELECT why FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');
SELECT why FROM sift._test_conn_take('dim_a_col');

-- the generation moves with every record
SELECT records AS recs FROM sift._test_conn_gen('dim_a_col') \gset
UPDATE dim_a SET category_id = category_id + 100 WHERE id = 70;
SELECT records > :recs AS moved FROM sift._test_conn_gen('dim_a_col');

-- a rebuilt parent index is another file with no journal
REINDEX INDEX dim_a_col;
SELECT why FROM sift._test_conn_take('dim_a_col');
SELECT count(*) AS journal_records FROM sift._test_col_log_entries('dim_a_col') WHERE kind = 4;

-- a key that is not an integer cannot be connected
CREATE TABLE dim_b (code text PRIMARY KEY, category_id int);
INSERT INTO dim_b VALUES ('a', 1);
CREATE INDEX dim_b_col ON dim_b USING sift_col (code, category_id);
SELECT sift._test_conn_enable('dim_b_col');
DROP TABLE dim_a, dim_b;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
