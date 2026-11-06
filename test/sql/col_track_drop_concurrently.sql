-- DROP INDEX CONCURRENTLY of a sift_col index that tracks its changes: its two triggers go with it, which assigns the transaction an xid that the command refuses ("must be first action in
-- transaction"). The error says what to do, and nothing is changed: the index is valid and keeps its triggers. With track_changes off first, or without CONCURRENTLY, the drop works.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 2000) i;
CREATE INDEX t_c ON t USING sift_col (id, g, v);
CREATE INDEX t_n ON t USING sift_col (id, g, v) WITH (track_changes = 'off');
CREATE FUNCTION state() RETURNS TABLE (idx text, valid boolean, triggers bigint) LANGUAGE sql AS $$
	SELECT c.relname::text, i.indisvalid, (SELECT count(*) FROM pg_trigger t JOIN pg_depend d ON d.classid = 'pg_trigger'::regclass AND d.objid = t.oid AND d.refobjid = c.oid
		WHERE t.tgrelid = 't'::regclass AND t.tgname LIKE 'sift_track_%')
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 't'::regclass AND c.relname LIKE 't\_%' ORDER BY 1 $$;
SELECT * FROM state();

\set VERBOSITY default
DROP INDEX CONCURRENTLY t_c;
SELECT * FROM state();
-- the tracking and the reads are untouched
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('t_c');

-- without tracking there is nothing to drop with it
DROP INDEX CONCURRENTLY t_n;
-- the workaround the hint gives
ALTER INDEX t_c SET (track_changes = 'off');
DROP INDEX CONCURRENTLY t_c;
SELECT * FROM state();

-- and without CONCURRENTLY
CREATE INDEX t_c ON t USING sift_col (id, g, v);
SELECT * FROM state();
DROP INDEX t_c;
SELECT * FROM state();
DROP TABLE t;
DROP FUNCTION state();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
