-- track_changes = 'auto' is the default: a new or reindexed sift_col index that holds the primary key of its table (one column) keeps the key and the tombstone log, installs the AFTER UPDATE and the
-- AFTER DELETE statement triggers on the table (one NOTICE says so) and writes the baseline; any other index is built as it was before 4.0 with one NOTICE that says why. 'off' keeps the old index;
-- an index whose options were written by an earlier release (none) stays inert until a REINDEX. The lifecycle of the triggers: made by CREATE INDEX, REINDEX and ALTER INDEX .. SET, dropped by DROP INDEX
-- and by turning the option off. Their names are sift_track_update_<index> and sift_track_delete_<index> with the oid of the trigger itself appended (PostgreSQL does it for an internal trigger; the oid
-- of the index is the trigger's argument), so a recreated trigger has another name.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 10, i, 'x' FROM generate_series(1, 3000) i;
CREATE FUNCTION triggers_of(tbl regclass) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce(string_agg(regexp_replace(tgname, '_[0-9]+$', ''), ' ' ORDER BY tgname), 'none') FROM pg_trigger WHERE tgrelid = tbl AND tgname LIKE 'sift%' $$;

-- the default: the primary key, the log segment, the key in the point directory beside what the formula chooses; both triggers, one NOTICE
CREATE INDEX t_dflt ON t USING sift_col (id, g, v);
SELECT split_part(o, '=', 1) AS opt, split_part(o, '=', 2) AS val FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_dflt' AND o ~ '^(track_changes|point_tid_columns)' ORDER BY 1;
SELECT count(*) AS logs FROM sift.col_segments('t_dflt') WHERE kind = 'tombstones';
SELECT triggers_of('t') AS triggers, complete, heap_free FROM sift.col_tracking('t_dflt');
-- REINDEX keeps the option and makes the triggers and the baseline again (other oids, so other names)
CREATE TEMP TABLE before_reindex AS SELECT tgname FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift%';
REINDEX INDEX t_dflt;
SELECT o AS after_reindex FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 't_dflt' AND o ~ '^track_changes';
SELECT (SELECT count(*) FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname IN (SELECT tgname FROM before_reindex)) AS kept_by_reindex, triggers_of('t') AS triggers, complete, heap_free FROM sift.col_tracking('t_dflt');
-- turned off by ALTER INDEX: the triggers go, and nothing is complete; turned on again by ALTER INDEX .. SET (a NOTICE, the baseline) and by RESET and REINDEX
ALTER INDEX t_dflt SET (track_changes = 'off');
SELECT triggers_of('t') AS after_off, complete, heap_free FROM sift.col_tracking('t_dflt');
ALTER INDEX t_dflt SET (track_changes = 'id');
SELECT triggers_of('t') AS after_set, complete, heap_free FROM sift.col_tracking('t_dflt');
ALTER INDEX t_dflt RESET (track_changes);
SELECT triggers_of('t') AS after_reset;
REINDEX INDEX t_dflt;
SELECT triggers_of('t') AS after_reindex_again;
-- DROP INDEX drops them with the index
DROP INDEX t_dflt;
SELECT triggers_of('t') AS after_drop;

-- the reasons for no key, each said once
CREATE TABLE nk (a int, b int);
INSERT INTO nk SELECT i, i FROM generate_series(1, 100) i;
CREATE INDEX nk_c ON nk USING sift_col (a, b);
SELECT count(*) AS logs, (SELECT coalesce(reloptions::text ~ 'track_changes', false) FROM pg_class WHERE relname = 'nk_c') AS has_option FROM sift.col_segments('nk_c') WHERE kind = 'tombstones';
DROP TABLE nk;
CREATE INDEX t_notheld ON t USING sift_col (g, v);
DROP INDEX t_notheld;
CREATE INDEX t_pt_off ON t USING sift_col (id, g, v) WITH (point_tid_columns = 'off');
DROP INDEX t_pt_off;
-- explicit off and a named key say nothing
CREATE INDEX t_off ON t USING sift_col (id, g, v) WITH (track_changes = 'off');
SELECT count(*) AS logs FROM sift.col_segments('t_off') WHERE kind = 'tombstones';
DROP INDEX t_off;
-- a column named for a table with no unique index on it yet is let through with a NOTICE (a dump restores the index before the key), reads check the heap until it has one
CREATE TABLE nk (a int NOT NULL, b int);
CREATE INDEX nk_d ON nk USING sift_col (a, b) WITH (track_changes = 'a');
DROP TABLE nk;

-- a partitioned table: one NOTICE for the partitions of a statement
CREATE TABLE p (a int, b int) PARTITION BY HASH (a);
CREATE TABLE p0 PARTITION OF p FOR VALUES WITH (modulus 3, remainder 0);
CREATE TABLE p1 PARTITION OF p FOR VALUES WITH (modulus 3, remainder 1);
CREATE TABLE p2 PARTITION OF p FOR VALUES WITH (modulus 3, remainder 2);
CREATE INDEX p_c ON p USING sift_col (a, b);
DROP TABLE p;
-- a table a statement trigger cannot hear (a partition: a statement on the parent fires no trigger of a child): the default is off with a NOTICE, a column named is an error
CREATE TABLE pk (id int PRIMARY KEY, g int NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE pk0 PARTITION OF pk FOR VALUES FROM (0) TO (100);
CREATE INDEX pk_auto ON pk USING sift_col (id, g);
CREATE INDEX pk_named ON pk USING sift_col (id, g) WITH (track_changes = 'id');
SELECT triggers_of('pk0') AS partition_triggers;
DROP TABLE pk;

-- an index of an earlier release has no option: it is inert (no tombstone), then a REINDEX gives it the key
CREATE INDEX t_old ON t USING sift_col (id, g, v);
ALTER INDEX t_old RESET (track_changes);
ALTER INDEX t_old RESET (point_tid_columns);
UPDATE t SET v = v + 1 WHERE id <= 100;
SELECT coalesce((SELECT rows FROM sift.col_segments('t_old') WHERE kind = 'tombstones'), 0) AS tombstones_inert;
REINDEX INDEX t_old;
UPDATE t SET v = v + 1 WHERE id <= 100;
SELECT (SELECT rows FROM sift.col_segments('t_old') WHERE kind = 'tombstones') AS tombstones_after_reindex;
DROP INDEX t_old;

-- three indexes on one table: every one gets a tombstone for each superseded entry, and the reads are the heap's
CREATE INDEX t_1 ON t USING sift_col (id, g, v);
CREATE INDEX t_2 ON t USING sift_col (id, v, pad);
CREATE INDEX t_3 ON t USING sift_col (id, g);
VACUUM (ANALYZE) t;
CREATE TEMP TABLE olds AS SELECT id, ctid AS old FROM t WHERE id % 7 = 0;
UPDATE t SET v = v + 1000 WHERE id % 7 = 0;
SELECT i AS index,
       (SELECT count(*) FROM sift._test_col_log_entries(i::regclass) e JOIN olds o ON o.old = e.tid) AS tombstones_of_the_old_versions,
       (SELECT count(*) FROM sift._test_col_log_entries(i::regclass)) AS entries
  FROM (VALUES ('t_1'), ('t_2'), ('t_3')) x(i) ORDER BY 1;
-- a row updated twice in a transaction that is rolled back: no tombstone stays
BEGIN; UPDATE t SET v = v + 1 WHERE id = 7; UPDATE t SET v = v + 1 WHERE id = 7; ROLLBACK;
SELECT i AS index, (SELECT count(*) FROM sift._test_col_log_entries(i::regclass)) AS entries_after_rollback FROM (VALUES ('t_1'), ('t_2'), ('t_3')) x(i) ORDER BY 1;
SET sift.enable_agg = on;
SET seq_page_cost = 10000;
SELECT g, count(*), sum(v) FROM t GROUP BY g ORDER BY g LIMIT 3;
SET sift.enable_agg = off;
RESET seq_page_cost;
SELECT g, count(*), sum(v) FROM t GROUP BY g ORDER BY g LIMIT 3;
SELECT sift.col_verify('t_1'), sift.col_verify('t_2'), sift.col_verify('t_3');
-- VACUUM drops the log with the rows it removes
VACUUM t;
SELECT i AS index, (SELECT count(*) FROM sift._test_col_log_entries(i::regclass)) AS entries_after_vacuum FROM (VALUES ('t_1'), ('t_2'), ('t_3')) x(i) ORDER BY 1;
DROP TABLE t CASCADE;
DROP FUNCTION triggers_of(regclass);
DROP TABLE olds;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
