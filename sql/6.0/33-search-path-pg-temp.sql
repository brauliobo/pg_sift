-- The functions that the earlier releases made fix their search_path without naming pg_temp, and PostgreSQL searches the temporary schema first for relations and types unless the path names it:
-- a temporary table or type that a session makes under the name of one a function reads takes its place. Every path ends in pg_temp from here on, as those of this release do.
DO $fix$
DECLARE
	f record;
BEGIN
	FOR f IN
		SELECT p.oid::pg_catalog.regprocedure AS fn, substr(c, 13) AS path
		FROM pg_catalog.pg_proc p
		CROSS JOIN LATERAL pg_catalog.unnest(p.proconfig) c
		WHERE p.pronamespace IN ('sift'::pg_catalog.regnamespace, 'sift_internal'::pg_catalog.regnamespace)
		  AND c LIKE 'search\_path=%' AND c !~ ', pg_temp$'
		  AND EXISTS (SELECT 1 FROM pg_catalog.pg_depend d JOIN pg_catalog.pg_extension e ON e.oid = d.refobjid
					  WHERE d.classid = 'pg_catalog.pg_proc'::pg_catalog.regclass AND d.objid = p.oid AND d.deptype = 'e' AND e.extname = 'pg_sift')
	LOOP
		EXECUTE pg_catalog.format('ALTER FUNCTION %s SET search_path = %s, pg_temp', f.fn, f.path);
	END LOOP;
END
$fix$;
