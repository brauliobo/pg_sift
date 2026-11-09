-- The library of a release that moved the triggers of track_changes (the index depends on them) on an extension that ALTER EXTENSION UPDATE has not brought there yet: sift.col_track_convert()
-- does not exist, and an index of an earlier release has its triggers depend on it. Every statement of a migration may run in that window: an index made now is dropped with its triggers, the
-- triggers of an earlier release's index are found (REINDEX and ALTER INDEX keep them, not make a second pair), and the update converts them without leaving two pairs.
CREATE EXTENSION pg_sift VERSION '4.2.1';
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET allow_system_table_mods = on;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 5, i FROM generate_series(1, 200) i;

-- the extension before the release: no sift.col_track_convert()
ALTER EXTENSION pg_sift UPDATE TO '4.2';
ALTER EXTENSION pg_sift DROP FUNCTION sift.col_track_convert();
DROP FUNCTION sift.col_track_convert();

-- the triggers of the table, those an index depends on (the form of this release) and those that depend on an index (the form of an earlier one)
CREATE FUNCTION state() RETURNS TABLE (triggers bigint, of_index bigint, on_index bigint, orphans bigint) LANGUAGE sql AS $$
	SELECT (SELECT count(*) FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_%'),
		(SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_trigger'::regclass
			AND d.objid IN (SELECT indexrelid FROM pg_index WHERE indrelid = 't'::regclass)),
		(SELECT count(*) FROM pg_depend d WHERE d.refclassid = 'pg_class'::regclass AND d.classid = 'pg_trigger'::regclass
			AND d.refobjid IN (SELECT indexrelid FROM pg_index WHERE indrelid = 't'::regclass)),
		(SELECT count(*) FROM pg_trigger g WHERE g.tgname LIKE 'sift_track_%' AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.refobjid = g.oid AND d.refclassid = 'pg_trigger'::regclass
			AND d.classid = 'pg_class'::regclass) AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.objid = g.oid AND d.classid = 'pg_trigger'::regclass AND d.refclassid = 'pg_class'::regclass
			AND d.refobjid IN (SELECT indexrelid FROM pg_index))) $$;
CREATE FUNCTION writes() RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	UPDATE t SET v = v + 1 WHERE id <= 10;
	DELETE FROM t WHERE id = (SELECT max(id) FROM t);
	INSERT INTO t SELECT max(id) + 1, 1, 1 FROM t;
	RETURN 'writes ok';
END $$;
-- what an earlier release made: the triggers depend on the index
CREATE FUNCTION make_legacy(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	WITH d AS (DELETE FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = idx AND refclassid = 'pg_trigger'::regclass AND deptype = 'n' RETURNING refobjid),
		i AS (INSERT INTO pg_depend SELECT 'pg_trigger'::regclass, refobjid, 0, 'pg_class'::regclass, idx, 0, 'a' FROM d RETURNING 1)
	SELECT count(*) FROM i $$;

-- an index made in the window, and dropped: both triggers go with it
CREATE INDEX t_new ON t USING sift_col (id, g, v);
SELECT * FROM state();
DROP INDEX t_new;
SELECT * FROM state();
SELECT writes();

-- an index of an earlier release: its triggers are found, a REINDEX and an ALTER INDEX keep them (one pair), and its log is trusted
CREATE INDEX t_old ON t USING sift_col (id, g, v);
SELECT make_legacy('t_old');
SELECT * FROM state();
SELECT triggers, complete, heap_free FROM sift.col_tracking('t_old');
REINDEX INDEX t_old;
SELECT * FROM state();
SELECT triggers, complete, heap_free FROM sift.col_tracking('t_old');
ALTER INDEX t_old SET (track_changes = 'id');
SELECT * FROM state();
SELECT writes();

-- the same index dropped in the window: its triggers go with it
SELECT make_legacy('t_old');
DROP INDEX t_old;
SELECT * FROM state();
SELECT writes();

-- the update converts the triggers of the indexes that stay, one pair each
CREATE INDEX t_old ON t USING sift_col (id, g, v);
SELECT make_legacy('t_old');
ALTER EXTENSION pg_sift UPDATE TO '4.2.1';
SELECT * FROM state();
SELECT triggers, complete, heap_free FROM sift.col_tracking('t_old');
SELECT writes();
DROP INDEX CONCURRENTLY t_old;
SELECT * FROM state();
SELECT writes();

DROP TABLE t;
DROP FUNCTION state(), writes(), make_legacy(regclass);
DROP EXTENSION pg_sift;
