-- The declared paths of a sift_table are keyed by its qualified name (sift.tblref), like the per-table options and the TTL: a restore without the table reads its name as 0 and the row is skipped,
-- and the rows of the others load. A table of 5.1 keeps regclass until this part runs; the view that reads it is made again, with the column of the type it has always had.
DO $$ BEGIN
	IF (SELECT atttypid FROM pg_attribute WHERE attrelid = 'sift.table_paths'::regclass AND attname = 'relid') = 'pg_catalog.regclass'::regtype THEN
		DROP VIEW sift.table_paths_maintained;
		ALTER TABLE sift.table_paths ALTER COLUMN relid TYPE sift.tblref USING relid::sift.tblref;
	END IF;
END $$;

CREATE OR REPLACE VIEW sift.table_paths_maintained WITH (security_barrier = true) AS
	SELECT p.relid::pg_catalog.regclass AS relid, p.attname, p.spec FROM sift.table_paths p WHERE has_table_privilege(p.relid, 'MAINTAIN');
COMMENT ON VIEW sift.table_paths_maintained IS 'The declared JSON paths of the sift_tables the caller owns or may maintain (relid, attname, spec); sift.table_paths lists every table and is for superusers.';
GRANT SELECT ON sift.table_paths_maintained TO PUBLIC;

CREATE OR REPLACE FUNCTION sift_internal.table_paths_skip() RETURNS trigger
	LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	IF NEW.relid::pg_catalog.oid = 0 THEN
		RETURN NULL;
	END IF;
	RETURN NEW;
END
$fn$;
COMMENT ON FUNCTION sift_internal.table_paths_skip() IS 'Internal: BEFORE ROW trigger of sift.table_paths, skips the row of a table that is not there (a restore without it)';
CREATE OR REPLACE TRIGGER table_paths_skip BEFORE INSERT OR UPDATE ON sift.table_paths
	FOR EACH ROW EXECUTE FUNCTION sift_internal.table_paths_skip();

-- The declared paths of a sift_table of Format 2.0 are leaves of its store, so a change of sift.table_paths, by whichever statement (sift.table_set_paths, a rename of a column, a restore), writes out
-- what the table holds under the old declaration and replaces its store in every backend. A table of Format 1 reads its declaration with its cache entry and is left alone.
CREATE OR REPLACE FUNCTION sift.table_paths_trg() RETURNS trigger
	AS 'MODULE_PATHNAME', 'sift_table_paths_trg' LANGUAGE C;
CREATE OR REPLACE TRIGGER table_paths_trg BEFORE INSERT OR UPDATE OR DELETE ON sift.table_paths
	FOR EACH ROW EXECUTE FUNCTION sift.table_paths_trg();
COMMENT ON FUNCTION sift.table_paths_trg() IS 'Trigger of sift.table_paths: a sift_table of Format 2.0 writes out the rows it holds under the old declaration of paths and replaces its store in every backend.';
