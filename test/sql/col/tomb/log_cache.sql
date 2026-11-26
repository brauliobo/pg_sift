-- The tombstone log of an index is cached by the backend that reads it, one context for each index. The cache must follow the life of the indexes: a backend that creates, uses and
-- drops indexes over and over (in one transaction, in many, with a rollback) keeps the entries of the indexes that exist, not one for every index it ever saw. The mutant keeps them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the tombstone log is Format 1.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
CREATE TABLE live (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO live SELECT i, i % 10, i FROM generate_series(1, 500) i;
CREATE INDEX live_c ON live USING sift_col (id, g, v) WITH (track_changes = 'id');
UPDATE live SET v = v + 1 WHERE id <= 10;
SELECT entries = 1 AS one_index FROM sift._test_col_log_cache();

-- one transaction: 60 tables with an index each, used by an update, dropped
DO $$
BEGIN
  FOR i IN 1..60 LOOP
    EXECUTE format('CREATE TABLE tmp_%s (id int PRIMARY KEY, g int NOT NULL, v int)', i);
    EXECUTE format('INSERT INTO tmp_%s SELECT x, x %% 10, x FROM generate_series(1, 50) x', i);
    EXECUTE format('CREATE INDEX tmp_%s_c ON tmp_%s USING sift_col (id, g, v) WITH (track_changes = ''id'')', i, i);
    EXECUTE format('UPDATE tmp_%s SET v = v + 1 WHERE id <= 5', i);
    EXECUTE format('DROP TABLE tmp_%s', i);
  END LOOP;
END $$;
UPDATE live SET v = v + 1 WHERE id <= 10;
SELECT entries <= 2 AS bounded_one_transaction, bytes < 1000000 AS small FROM sift._test_col_log_cache();

-- one transaction each, and a rolled back build
CREATE TABLE loop_t (id int PRIMARY KEY, g int NOT NULL, v int);
INSERT INTO loop_t SELECT x, x % 10, x FROM generate_series(1, 50) x;
SELECT s FROM generate_series(1, 40) i, unnest(ARRAY['CREATE INDEX loop_c ON loop_t USING sift_col (id, g, v) WITH (track_changes = ''id'')',
                                                     'UPDATE loop_t SET v = v + 1 WHERE id <= 5', 'DROP INDEX loop_c']) WITH ORDINALITY u(s, n) ORDER BY i, n
\gexec
BEGIN;
CREATE INDEX loop_c ON loop_t USING sift_col (id, g, v) WITH (track_changes = 'id');
UPDATE loop_t SET v = v + 1 WHERE id <= 5;
ROLLBACK;
DROP TABLE loop_t;
UPDATE live SET v = v + 1 WHERE id <= 10;
SELECT entries <= 2 AS bounded_many_transactions FROM sift._test_col_log_cache();

-- REINDEX gives the index a new file: the same entry, not another
REINDEX INDEX live_c;
UPDATE live SET v = v + 1 WHERE id <= 10;
SELECT entries = 1 AS still_one FROM sift._test_col_log_cache();

-- the mutant keeps the entries of the dropped indexes, so the proof above can fail; and the reclaim finds them again once it is off
SET sift.test_no_log_reap = on;
DO $$
BEGIN
  FOR i IN 1..20 LOOP
    EXECUTE format('CREATE TABLE tmp_%s (id int PRIMARY KEY, g int NOT NULL, v int)', i);
    EXECUTE format('CREATE INDEX tmp_%s_c ON tmp_%s USING sift_col (id, g, v) WITH (track_changes = ''id'')', i, i);
    EXECUTE format('DROP TABLE tmp_%s', i);
  END LOOP;
END $$;
UPDATE live SET v = v + 1 WHERE id <= 10;
SELECT entries >= 20 AS mutant_grows FROM sift._test_col_log_cache();
RESET sift.test_no_log_reap;
UPDATE live SET v = v + 1 WHERE id <= 10;
SELECT entries <= 2 AS reclaimed FROM sift._test_col_log_cache();

-- a log of many entries: a backend that appends to it caches none, what asks only what the log vouches for reads none of its entries, and a pass over the entries (the fold
-- of the marks, VACUUM) keeps what sift.backend_cache_kb allows, the rest in sorted runs on temp files; the rows a scan reads are the same at 1 GB and at 16 kB. The setting is the
-- mutant of the bound: one that holds the whole log (1 GB) caches it
CREATE TABLE big (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO big SELECT i, i % 10, i FROM generate_series(1, 30000) i;
CREATE INDEX big_c ON big USING sift_col (id, g, v) WITH (track_changes = 'id');
\c -
SET sift.backend_cache_kb = '1GB';
UPDATE big SET v = v + 1;
UPDATE big SET v = v + 1 WHERE id % 2 = 0;
SELECT bytes < 100000 AS writer_caches_none FROM sift._test_col_log_cache();
SELECT count(*) AS tracking_rows FROM sift.col_tracking('big_c');
SELECT bytes < 100000 AS header_reads_no_entries FROM sift._test_col_log_cache();
\c -
SET sift.backend_cache_kb = '1GB';
SET enable_seqscan = off;
SELECT count(*) AS n_whole, sum(v) AS s_whole FROM big WHERE g IN (3, 4) AND id < 20000 \gset
SELECT bytes > 1000000 AS mutant_holds_the_log FROM sift._test_col_log_cache();
\c -
SET sift.backend_cache_kb = '16kB';
SET enable_seqscan = off;
SELECT count(*) = :n_whole AND sum(v) = :s_whole AS same_rows_in_runs FROM big WHERE g IN (3, 4) AND id < 20000;
\c -
SET sift.test_create_format1 = on;
SET sift.backend_cache_kb = '16kB';
SELECT sift._test_col_fold_marks('big_c'::regclass) >= 0 AS folded;
SELECT bytes < 300000 AS bounded_by_the_limit FROM sift._test_col_log_cache();
VACUUM big;
SELECT count(*) = :n_whole AND sum(v) = :s_whole AS same_rows_after_vacuum FROM big WHERE g IN (3, 4) AND id < 20000;
DROP TABLE big;

-- the entry of a log is held while its directory is read: the read of a chained directory asks the budget of the caches for room, and that must not take the entry it is filling.
-- Each VACUUM is the first use of the log in a backend of its own, with a budget from one that holds hardly anything to one that holds the directory
CREATE TABLE chained (id int PRIMARY KEY, g int NOT NULL, pad text) WITH (autovacuum_enabled = false);
SET client_min_messages = warning;
SET sift.test_dir_flat_max = 1;
SET sift.test_col_segment_kb = 16;
INSERT INTO chained SELECT i, i % 50, md5(i::text) FROM generate_series(1, 20000) i;
CREATE INDEX chained_c ON chained USING sift_col (id, g, pad) WITH (track_changes = 'ctid');
SELECT chained, nsegs > 1 AS segments FROM sift._test_dir_info('chained_c');
\c -
SET sift.backend_cache_kb = '8kB';
DELETE FROM chained WHERE id % 7 = 0;
VACUUM chained;
\c -
SET sift.backend_cache_kb = '24kB';
DELETE FROM chained WHERE id % 7 = 1;
VACUUM chained;
\c -
SET sift.backend_cache_kb = '64kB';
DELETE FROM chained WHERE id % 7 = 2;
VACUUM chained;
\c -
SELECT quarantined FROM sift.status('chained_c');
SELECT count(*) AS rows_left FROM chained WHERE id > 0;
DROP TABLE chained;

DROP TABLE live;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
