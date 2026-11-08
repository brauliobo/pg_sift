-- The index depends on its two track_changes triggers (they do not depend on it), so DROP INDEX CONCURRENTLY has nothing to delete first and finds no transaction id; the transaction
-- that deletes the index removes the triggers before it commits (inside that transaction they are still there: the state shown after a DROP INDEX in a block). Every way an index goes leaves no trigger behind and no trigger that fires into a missing index, a rollback leaves
-- them, and the writes of the table go on after each. The sessions that fail while a writer holds the table, and the writers beside a drop, are in test/t/col_track_drop_concurrently.sh.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 2000) i;

-- the indexes of the table that are valid, the triggers an index depends on, the triggers on the table, and those that no index depends on
CREATE FUNCTION state() RETURNS TABLE (idxs text, of_index bigint, on_table bigint, orphans bigint) LANGUAGE sql AS $$
	SELECT (SELECT coalesce(string_agg(c.relname || CASE WHEN i.indisvalid THEN '' ELSE '(invalid)' END, ',' ORDER BY c.relname), '-') FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
			WHERE i.indrelid = 't'::regclass AND c.relname <> 't_pkey'),
		(SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_trigger'::regclass
			AND d.objid IN (SELECT indexrelid FROM pg_index WHERE indrelid = 't'::regclass)),
		(SELECT count(*) FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_%'),
		(SELECT count(*) FROM pg_trigger g WHERE g.tgname LIKE 'sift_track_%' AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.refclassid = 'pg_trigger'::regclass AND d.refobjid = g.oid
			AND d.classid = 'pg_class'::regclass)) $$;
-- an UPDATE, a DELETE and an INSERT: each fires the triggers that are left
CREATE FUNCTION writes() RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	UPDATE t SET v = v + 1 WHERE id <= 10;
	DELETE FROM t WHERE id = (SELECT max(id) FROM t);
	INSERT INTO t SELECT max(id) + 1, 1, 1 FROM t;
	RETURN 'writes ok';
END $$;

CREATE TABLE cases (n int, label text, pre text, stmt text);
INSERT INTO cases VALUES
	(1, 'drop index concurrently',           'SET client_min_messages = warning', 'DROP INDEX CONCURRENTLY t_c'),
	(2, 'drop index concurrently if exists', 'SET client_min_messages = warning', 'DROP INDEX CONCURRENTLY IF EXISTS t_c'),
	(3, 'drop index',                        'SET client_min_messages = warning', 'DROP INDEX t_c'),
	(4, 'drop index if exists',              'SET client_min_messages = warning', 'DROP INDEX IF EXISTS t_c'),
	(5, 'drop the column of the index',      'SET client_min_messages = warning', 'ALTER TABLE t DROP COLUMN v'),
	(6, 'track_changes off, then drop',      'ALTER INDEX t_c SET (track_changes = ''off'')', 'DROP INDEX CONCURRENTLY t_c');

-- each case: a tracked index with its two triggers, the statement, the state after it, the writes
SELECT s FROM cases c, LATERAL unnest(ARRAY[
	'CREATE INDEX t_c ON t USING sift_col (id, g, v)',
	format('SELECT %L AS "case"', c.label),
	'SELECT * FROM state()',
	c.pre,
	c.stmt,
	'SELECT * FROM state()',
	'DROP INDEX IF EXISTS t_c',
	'ALTER TABLE t ADD COLUMN IF NOT EXISTS v int NOT NULL DEFAULT 0',
	'SELECT writes()']) WITH ORDINALITY u(s, o)
	ORDER BY c.n, o
\gexec

-- a rollback of the drop leaves the index and its triggers
CREATE INDEX t_c ON t USING sift_col (id, g, v);
BEGIN;
DROP INDEX t_c;
SELECT * FROM state();
ROLLBACK;
SELECT * FROM state();
SELECT writes();
SELECT triggers, complete FROM sift.col_tracking('t_c');

-- REINDEX CONCURRENTLY keeps the two triggers, which then belong to the index that stays
REINDEX INDEX CONCURRENTLY t_c;
SELECT * FROM state();
SELECT writes();

-- the swap: a concurrent copy, the old one dropped concurrently, the copy renamed; each index has its own two triggers while both exist
CREATE INDEX CONCURRENTLY t_new ON t USING sift_col (id, g, v);
SELECT * FROM state();
DROP INDEX CONCURRENTLY t_c;
SELECT * FROM state();
ALTER INDEX t_new RENAME TO t_c;
SELECT * FROM state();
SELECT writes();
SELECT triggers FROM sift.col_tracking('t_c');
REINDEX INDEX t_c;
SELECT triggers, complete, heap_free FROM sift.col_tracking('t_c');

-- the other index of the table is not touched by dropping an index
CREATE INDEX t_other ON t USING sift_col (id, g, v);
SELECT * FROM state();
DROP INDEX CONCURRENTLY t_other;
SELECT * FROM state();
SELECT writes();

-- DROP INDEX CONCURRENTLY cannot run in a transaction block, and says so before it changes anything
BEGIN;
DROP INDEX CONCURRENTLY t_c;
ROLLBACK;
SELECT * FROM state();

-- sift.col_track_convert() has nothing left to convert
SELECT sift.col_track_convert();

-- a trigger of the index cannot be dropped alone
DO $$ DECLARE n text; BEGIN SELECT tgname INTO n FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_delete%';
	EXECUTE format('DROP TRIGGER %I ON t', n);
	RAISE EXCEPTION 'the trigger was dropped';
EXCEPTION WHEN dependent_objects_still_exist THEN NULL; END $$;

-- DROP TABLE, and DROP EXTENSION .. CASCADE, take the index and its triggers
DROP TABLE t;
SELECT count(*) AS triggers_left FROM pg_trigger WHERE tgname LIKE 'sift_track_%';
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL);
CREATE INDEX t_c ON t USING sift_col (id, g, v);
CREATE INDEX t_d ON t USING sift_col (id, g, v);
SELECT count(*) AS triggers FROM pg_trigger WHERE tgname LIKE 'sift_track_%';
DROP EXTENSION pg_sift CASCADE;
SELECT count(*) AS triggers_left FROM pg_trigger WHERE tgname LIKE 'sift_track_%';
SELECT count(*) AS indexes_left FROM pg_class WHERE relname IN ('t_c', 't_d');
DROP TABLE t;
DROP FUNCTION state();
DROP FUNCTION writes();
DROP TABLE cases;
