# sift.col_track_convert() (ALTER EXTENSION UPDATE) rewrites the dependencies between an index of an earlier release and its two triggers. A DROP INDEX that is
# running at the time deletes those rows and the triggers: the update must wait for the table's lock first and leave no dependency on a deleted trigger or index.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_cv (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
	INSERT INTO iso_cv SELECT i, i % 5, i FROM generate_series(1, 200) i;
	CREATE INDEX iso_cv_a ON iso_cv USING sift_col (id, g, v);
	CREATE INDEX iso_cv_b ON iso_cv USING sift_col (id, g, v);
	-- what an earlier release made: the triggers depend on their index
	SET allow_system_table_mods = on;
	WITH d AS (DELETE FROM pg_depend WHERE classid = 'pg_class'::regclass AND refclassid = 'pg_trigger'::regclass AND deptype = 'n'
			AND objid IN ('iso_cv_a'::regclass, 'iso_cv_b'::regclass) RETURNING objid, refobjid)
	INSERT INTO pg_depend SELECT 'pg_trigger'::regclass, refobjid, 0, 'pg_class'::regclass, objid, 0, 'a' FROM d;

	CREATE FUNCTION iso_cv_state(OUT triggers int, OUT depend_on_trigger int, OUT dangling int) LANGUAGE sql AS $$
		SELECT (SELECT count(*)::int FROM pg_trigger WHERE tgrelid = 'iso_cv'::regclass AND tgname LIKE 'sift_track_%'),
			(SELECT count(*)::int FROM pg_depend WHERE classid = 'pg_class'::regclass AND refclassid = 'pg_trigger'::regclass),
			(SELECT count(*)::int FROM pg_depend d WHERE (d.refclassid = 'pg_trigger'::regclass AND NOT EXISTS (SELECT 1 FROM pg_trigger t WHERE t.oid = d.refobjid))
				OR (d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_trigger'::regclass AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.objid))) $$;
}

teardown
{
	DROP TABLE iso_cv;
	DROP FUNCTION iso_cv_state();
}

session drop
step ddrop	{ BEGIN; DROP INDEX iso_cv_a; }
step dend	{ COMMIT; }

session convert
step cconvert	{ SELECT sift.col_track_convert() AS converted; }

session check
step cstate	{ SELECT * FROM iso_cv_state(); }

permutation ddrop cconvert dend cstate
