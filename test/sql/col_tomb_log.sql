-- The tombstone log of a heap column index (directory kind SIFT_SEG_TOMB): a lane chain of entries, one for a heap address that stopped being live, kept apart from the write
-- buffers. It is created with the segment, appended to by transactions, listed in log order, dropped entry by entry when VACUUM removes the entry's row from the index (the heap may
-- then give the address to another row), and it does not disturb the reads, the counts or the checks of the index.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 10 FROM generate_series(1, 2000) i;
CREATE INDEX t_c ON t USING sift_col (id, v) WITH (track_updates = 'off');
VACUUM (ANALYZE) t;

-- no log until it is asked for; one segment of its own kind after
SELECT count(*) AS logs FROM sift.col_segments('t_c') WHERE kind = 'tombstones';
SELECT sift._test_col_log_create('t_c');
SELECT sift._test_col_log_create('t_c');
SELECT count(*) AS logs, bool_and(rows = 0) AS empty FROM sift.col_segments('t_c') WHERE kind = 'tombstones';

-- entries come back as appended, with the transaction and the command that wrote them
CREATE TEMP TABLE olds AS SELECT id, ctid AS t FROM t WHERE id IN (5, 6, 7, 8) ORDER BY id;
BEGIN;
SELECT sift._test_col_log_append('t_c', t) FROM olds ORDER BY id;
SELECT count(*) AS entries, bool_and(xid::text::bigint = txid_current() % 4294967296) AS mine, count(DISTINCT cid) AS commands
  FROM sift._test_col_log_entries('t_c');
COMMIT;
SELECT array_agg(e.tid ORDER BY o.id) = (SELECT array_agg(t ORDER BY id) FROM olds) AS same_addresses_in_order
  FROM sift._test_col_log_entries('t_c') e JOIN olds o ON o.t = e.tid;

-- the reads are those of the heap, with and without the pushdown, and the checks pass
CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS $$ SELECT count(*) || '/' || sum(id) || '/' || sum(v) FROM t $$;
SET sift.enable_agg = on;
SELECT answers() AS agg;
SET sift.enable_agg = off;
SELECT answers() AS heap;
RESET sift.enable_agg;
SELECT sift.col_verify('t_c') IS NOT NULL AS verified;

-- VACUUM removes the entries of three rows from the index: their tombstones go with them; the fourth row lives and keeps its entry
DELETE FROM t WHERE id IN (5, 6, 7);
VACUUM (INDEX_CLEANUP ON) t;
SELECT count(*) AS entries, bool_and(e.tid = (SELECT t FROM olds WHERE id = 8)) AS the_live_row
  FROM sift._test_col_log_entries('t_c') e;
SELECT sift.col_verify('t_c') IS NOT NULL AS verified;
SELECT answers() AS heap_after;
SET sift.enable_agg = on;
SELECT answers() AS agg_after;
RESET sift.enable_agg;

-- a REINDEX builds an index with no log: the old file, and with it the log, is gone
REINDEX INDEX t_c;
SELECT count(*) AS logs FROM sift.col_segments('t_c') WHERE kind = 'tombstones';
DROP FUNCTION answers();
DROP TABLE olds;
DROP TABLE t CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
