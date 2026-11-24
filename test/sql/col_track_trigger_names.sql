-- The names of the triggers of an index with track_changes: PostgreSQL gives an internal trigger the name "<name>_<oid of the trigger>" in a buffer of NAMEDATALEN bytes, so the name this library
-- gives must leave room for the oid, or two indexes of one table whose names share a long prefix get the same name and the second CREATE INDEX fails on pg_trigger_tgrelid_tgname_index. Both formats:
-- one trigger of both events for an index of format major 2, a trigger for each event for any other.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.test_create_format1 = off;
CREATE TABLE t2 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO t2 SELECT i, i % 10, i FROM generate_series(1, 500) i;
CREATE TABLE t1 (LIKE t2 INCLUDING ALL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO t1 SELECT * FROM t2;
CREATE TEMP TABLE expected (tid tid);
-- the addresses of the log that no aborted transaction wrote, against the addresses the oracle expects (an index of format major 1)
CREATE FUNCTION log_matches(idx regclass) RETURNS bool LANGUAGE sql AS $$
  SELECT coalesce((SELECT array_agg(DISTINCT tid ORDER BY tid) FROM expected), '{}') =
         coalesce((SELECT array_agg(DISTINCT tid ORDER BY tid) FROM sift._test_col_log_entries(idx) WHERE txid_status(xid::text::bigint) IS DISTINCT FROM 'aborted'), '{}') $$;
-- the answer of the index to a read of t2 is the heap's
CREATE FUNCTION same_answers() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE h record; i record;
BEGIN
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
	SELECT count(*) AS c, sum(v) AS s INTO h FROM t2 WHERE g = 3;
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = on; SET LOCAL enable_bitmapscan = on;
	SELECT count(*) AS c, sum(v) AS s INTO i FROM t2 WHERE g = 3;
	RETURN h = i;
END $$;
-- the triggers of a table: how many, how many names, whether all fit a name, and how many do not end in their own oid (a name cut short loses it)
CREATE FUNCTION names(tbl regclass) RETURNS TABLE (triggers bigint, distinct_names bigint, fits boolean, without_own_oid bigint) LANGUAGE sql AS
$$ SELECT count(*), count(DISTINCT tgname), coalesce(max(length(tgname)) <= 63, true), count(*) FILTER (WHERE tgname !~ ('_' || oid || '$'))
     FROM pg_trigger WHERE tgrelid = tbl AND tgname LIKE 'sift_track_%' $$;

-- the collision: names that share their first 50 bytes
SELECT length('t_track_changes_index_with_a_name_longer_than_a_trigger_name_a') AS name_length;
CREATE INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_a ON t2 USING sift_col (id, g, v);
CREATE INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_a1 ON t2 USING sift_col (id, g, v);
SELECT * FROM names('t2');
SELECT * FROM sift.col_tracking('t_track_changes_index_with_a_name_longer_than_a_trigger_name_a') UNION ALL SELECT * FROM sift.col_tracking('t_track_changes_index_with_a_name_longer_than_a_trigger_name_a1');

-- DELETE and UPDATE of the key leave the reads as the heap's
DELETE FROM t2 WHERE id BETWEEN 1 AND 100;
UPDATE t2 SET id = id + 1000, g = 3 WHERE id BETWEEN 101 AND 200;
SELECT same_answers();
SELECT * FROM sift.col_tracking('t_track_changes_index_with_a_name_longer_than_a_trigger_name_a') UNION ALL SELECT * FROM sift.col_tracking('t_track_changes_index_with_a_name_longer_than_a_trigger_name_a1');

-- dropping one leaves the triggers of the other, dropping the last leaves none
DROP INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_a;
SELECT * FROM names('t2');
REINDEX INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_a1;
SELECT * FROM names('t2'), sift.col_tracking('t_track_changes_index_with_a_name_longer_than_a_trigger_name_a1');
DROP INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_a1;
SELECT * FROM names('t2');

-- an index of format major 1 has a trigger for each event, and its log has the rows they changed
SET sift.test_create_format1 = on;
CREATE INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_b ON t1 USING sift_col (id, g, v);
CREATE INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_b1 ON t1 USING sift_col (id, g, v);
SET sift.test_create_format1 = off;
SELECT * FROM names('t1');
INSERT INTO expected SELECT ctid FROM t1 WHERE id BETWEEN 1 AND 100;
DELETE FROM t1 WHERE id BETWEEN 1 AND 50;
UPDATE t1 SET id = id + 1000 WHERE id BETWEEN 51 AND 100;
SELECT log_matches('t_track_changes_index_with_a_name_longer_than_a_trigger_name_b') AS first, log_matches('t_track_changes_index_with_a_name_longer_than_a_trigger_name_b1') AS second;
DROP INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_b;
SELECT * FROM names('t1');
DROP INDEX t_track_changes_index_with_a_name_longer_than_a_trigger_name_b1;
SELECT * FROM names('t1');

-- a name of multibyte characters is cut between two of them (the name of the trigger is text of the database encoding)
SELECT format('CREATE INDEX %I ON t2 USING sift_col (id, g, v)', 't_' || repeat('ñ', 30)) AS stmt \gexec
SELECT triggers, fits, without_own_oid, convert_from((SELECT tgname::text::bytea FROM pg_trigger WHERE tgrelid = 't2'::regclass), 'UTF8') ~ '^sift_track_t_ñ+_[0-9]+$' AS valid_text FROM names('t1');

-- the journal index a scope index makes (of format major 1: its triggers have the longer names) is named <table>_<member>_<key>_sift_journal, then ..journal1 when that name is taken: one shared prefix, so the triggers of both are told apart by what follows
CREATE TABLE entity_parents (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false);
INSERT INTO entity_parents SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
CREATE TABLE events (id int PRIMARY KEY, entity_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, i % 10 FROM generate_series(1, 2000) i;
SET sift.test_create_format1 = on;
CREATE INDEX entity_parents_entity_id_parent_id_sift_journal ON entity_parents USING sift_col (parent_id, entity_id) WITH (track_changes = 'ctid');
SET client_min_messages = notice;
CREATE INDEX events_col ON events USING sift_col (id, entity_id, v) WITH (scope = 'parent_id: entity_id -> entity_parents(entity_id, parent_id) max_multiplicity(8)');
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SELECT c.relname FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid WHERE i.indrelid = 'entity_parents'::regclass AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col') ORDER BY 1;
SELECT * FROM names('entity_parents');
DELETE FROM entity_parents WHERE entity_id <= 10;
UPDATE entity_parents SET parent_id = parent_id + 100 WHERE entity_id BETWEEN 11 AND 20;
INSERT INTO entity_parents VALUES (1, 99);
DROP INDEX events_col;
SELECT c.relname FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid WHERE i.indrelid = 'entity_parents'::regclass AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col') ORDER BY 1;
SELECT * FROM names('entity_parents');
DROP TABLE entity_parents, events;
DROP TABLE t1, t2;
SELECT count(*) AS triggers_left FROM pg_trigger WHERE tgname LIKE 'sift_track_%';
DROP FUNCTION names(regclass), same_answers(), log_matches(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
