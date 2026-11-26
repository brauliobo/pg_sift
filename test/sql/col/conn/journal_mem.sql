-- The key journal of an index is read record by record: the changed keys of a read (sift_conn_changed) and the age check of VACUUM walk the records of the log, those past
-- sift.backend_cache_kb from its sorted runs, and hold one block of a run whatever the number of records. The scope of the walk (col_conn_changed) holds the same bytes at every
-- size of the journal (the table of the distinct keys is the result's, 50 keys here), and the keys are the same in memory and in runs. A walk that gathers the records of the key
-- into one array (sift_tomb_find) holds bytes a record: this test fails on it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the connection stamps and journals are Format 1.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.backend_cache_kb = '16kB';
CREATE TABLE dim (id int PRIMARY KEY, c int NOT NULL, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 10, 'l' || i FROM generate_series(1, 50) i;
CREATE INDEX dim_col ON dim USING sift_col (id, c, label);
SELECT sift._test_conn_enable('dim_col') <> 0 AS journal;
SELECT lineage AS lin, horizon AS hor FROM sift._test_conn_take('dim_col') \gset

CREATE TABLE census (records int, peak int8, cache_growth int8);
-- the log is read in full first (the cache holds what the limit allows of it), then the changed keys: the walk adds nothing to the cache of the log and holds the bytes of its scope
CREATE FUNCTION census_step(lin int8, hor xid) RETURNS void LANGUAGE plpgsql AS $$
DECLARE cache int8;
BEGIN
  PERFORM count(*) FROM sift._test_col_log_entries('dim_col');
  SELECT bytes INTO cache FROM sift._test_col_log_cache();
  PERFORM count(*) FROM sift._test_mem_highwater(true);
  PERFORM count(*) FROM sift._test_conn_changed('dim_col', lin, hor, 100);
  INSERT INTO census SELECT (SELECT records FROM sift._test_conn_gen('dim_col')), peak, (SELECT bytes FROM sift._test_col_log_cache()) - cache
    FROM sift._test_mem_highwater() WHERE scope = 'col_conn_changed';
END $$;
-- 100 records a statement, of the same 50 keys: 800, 1600, 2400 and 3200 records
SELECT 'UPDATE dim SET c = c + 1' FROM generate_series(1, 8) \gexec
SELECT census_step(:lin, :'hor');
SELECT 'UPDATE dim SET c = c + 1' FROM generate_series(1, 8) \gexec
SELECT census_step(:lin, :'hor');
SELECT 'UPDATE dim SET c = c + 1' FROM generate_series(1, 8) \gexec
SELECT census_step(:lin, :'hor');
SELECT 'UPDATE dim SET c = c + 1' FROM generate_series(1, 8) \gexec
SELECT census_step(:lin, :'hor');
SELECT records FROM census ORDER BY records;
SELECT count(DISTINCT peak) = 1 AS same_bytes_at_every_size, max(peak) < 32768 AS under_the_budget, max(cache_growth) = 0 AS nothing_cached FROM census;

-- a read that finds more keys than its cap stops there; the walk holds the same
SELECT count(*) AS reset FROM sift._test_mem_highwater(true) \gset
SELECT why FROM sift._test_conn_changed('dim_col', :lin, :'hor', 10);
SELECT count(*) = 1 AS overflow_holds_the_same FROM sift._test_mem_highwater() WHERE scope = 'col_conn_changed' AND peak = (SELECT max(peak) FROM census);

-- the same keys from the runs and from memory (a fresh backend with the whole log in memory)
SELECT md5(string_agg(key::text, ',' ORDER BY key)) AS keys_in_runs FROM sift._test_conn_changed('dim_col', :lin, :'hor', 100) \gset
\c -
SET sift.backend_cache_kb = '1GB';
SELECT md5(string_agg(key::text, ',' ORDER BY key)) = :'keys_in_runs' AS same_keys_in_memory FROM sift._test_conn_changed('dim_col', :lin, :'hor', 100);

-- VACUUM walks the journal for the records the age limit takes
SET sift.backend_cache_kb = '16kB';
VACUUM dim;
SELECT count(*) AS keys_after_vacuum FROM sift._test_conn_changed('dim_col', :lin, :'hor', 100) WHERE key IS NOT NULL;

DROP FUNCTION census_step;
DROP TABLE census, dim;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
