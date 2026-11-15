-- A parent index with a connected child index never loses its key journal: CREATE INDEX of the connection makes it, REINDEX and REINDEX CONCURRENTLY make it again for the index they
-- build (the stamps of the old one are another lineage, so nothing derived before is read), and the child index depends on the parent index.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 10 FROM generate_series(1, 1000) i;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 1000, i FROM generate_series(1, 5000) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id);
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, value_num);
CREATE FUNCTION lineage_of(idx regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT lineage FROM sift._test_conn_gen(idx) $$;

-- an unconnected parent has no journal after a REINDEX
REINDEX INDEX dim_a_col;
SELECT lineage_of('dim_a_col') = 0 AS no_journal;

SELECT sift._test_conn_connect('events_col', 'dim_a_col');
SELECT lineage_of('dim_a_col') <> 0 AS journal;
SELECT count(*) AS child_depends FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_col'::regclass AND refobjid = 'dim_a_col'::regclass AND deptype = 'n';
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_a_col') \gset
SELECT lineage_of('dim_a_col') AS old_lineage \gset

-- REINDEX: the same index, a journal again, another lineage; the old stamp is not vouched, a new one is
REINDEX INDEX dim_a_col;
SELECT lineage_of('dim_a_col') <> 0 AS journal, lineage_of('dim_a_col') <> :old_lineage AS other_lineage;
SELECT why FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');
SELECT why FROM sift._test_conn_take('dim_a_col');
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_a_col') \gset
UPDATE dim_a SET category_id = 99 WHERE id = 5;
SELECT why, key FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');

-- REINDEX CONCURRENTLY: the copy has a journal before writers use it; the log is not complete until a VACUUM makes it so, and nothing is vouched meanwhile
SELECT lineage_of('dim_a_col') AS old_lineage \gset
REINDEX INDEX CONCURRENTLY dim_a_col;
SELECT lineage_of('dim_a_col') <> 0 AS journal, lineage_of('dim_a_col') <> :old_lineage AS other_lineage;
SELECT count(*) AS child_depends FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_col'::regclass AND refobjid = 'dim_a_col'::regclass AND deptype = 'n';
SELECT why FROM sift._test_conn_take('dim_a_col');
UPDATE dim_a SET category_id = 98 WHERE id = 6;
VACUUM dim_a;
SELECT why FROM sift._test_conn_take('dim_a_col');
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_a_col') \gset
UPDATE dim_a SET category_id = 97 WHERE id = 7;
SELECT why, key FROM sift._test_conn_changed('dim_a_col', :lin, :'hor');

-- the parent index cannot be dropped under the child index; dropping the child frees it
DROP INDEX dim_a_col;
DROP INDEX events_col;
DROP INDEX dim_a_col;
DROP TABLE events, dim_a;
DROP FUNCTION lineage_of(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
