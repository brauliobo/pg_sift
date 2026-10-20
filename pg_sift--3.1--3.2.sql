\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.2'" to load this file. \quit

-- Paths of a jsonb column of a sift_table (docs/COL-JSONB-31.md section 4): a table access method has no options, so the declaration is a
-- row here, by table and column name, in the grammar of the `paths` option of sift_col for that one column. A change applies to the segments
-- written afterwards (promotion, merge, VACUUM FULL); until every segment has the paths a query reads the column as before.
CREATE TABLE sift.table_paths (relid regclass NOT NULL, attname name NOT NULL, spec text NOT NULL, PRIMARY KEY (relid, attname));
SELECT pg_catalog.pg_extension_config_dump('sift.table_paths', '');

-- Checks that the table can hold the paths and rebuilds its layout (C).
CREATE FUNCTION sift.table_paths_check(tbl regclass, col name, spec text) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_table_paths_check' LANGUAGE C VOLATILE;

-- Declares the paths of column col of the sift_table tbl; an empty or NULL spec removes the declaration.
CREATE FUNCTION sift.table_set_paths(tbl regclass, col name, spec text) RETURNS void
	LANGUAGE plpgsql VOLATILE AS $fn$
BEGIN
	PERFORM sift.table_paths_check(tbl, col, spec);
	IF coalesce(spec, '') = '' THEN
		DELETE FROM sift.table_paths WHERE relid = tbl AND attname = col;
	ELSE
		INSERT INTO sift.table_paths VALUES (tbl, col, spec) ON CONFLICT (relid, attname) DO UPDATE SET spec = EXCLUDED.spec;
	END IF;
END
$fn$;

-- The declaration goes with the table.
CREATE FUNCTION sift.table_paths_drop() RETURNS event_trigger
	LANGUAGE plpgsql AS $fn$
BEGIN
	DELETE FROM sift.table_paths p USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND p.relid = d.objid;
END
$fn$;

CREATE EVENT TRIGGER sift_table_paths_drop ON sql_drop EXECUTE FUNCTION sift.table_paths_drop();
