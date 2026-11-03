-- The tombstone log of an index is cached by the backend that reads it, one context for each index. The cache must follow the life of the indexes: a backend that creates, uses and
-- drops indexes over and over (in one transaction, in many, with a rollback) keeps the entries of the indexes that exist, not one for every index it ever saw. The mutant keeps them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
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

DROP TABLE live;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
