-- Secondary projections of a sift_col index or sift_table of Format 2.0: a second sort order of the rows, or a pre-aggregate by part, kept beside them and chosen by the planner when it answers
-- a query for less. They are not the named projections of 5.0 (sift.projections, sift.projection_create): that table keeps its name, and this part lists its own in sift.secondary_projections.

-- The definitions the projections of the directory are checked against: a projection with no definition, or one that no longer parses against the relation, is stale and never read.
-- The key of a relation is written as its qualified name, like the per-table options (sift.tblref). rel_name is that name as a text: a restore loads the rows before it builds the sift_col
-- indexes (post-data), so the row of an index reads rel as 0 and keeps rel_name until the CREATE INDEX of that name binds it (sift_internal.projection_defs_bind).
CREATE TABLE IF NOT EXISTS sift.projection_defs (rel sift.tblref NOT NULL, name text NOT NULL, spec text NOT NULL, rel_name text NOT NULL DEFAULT '');
CREATE UNIQUE INDEX IF NOT EXISTS projection_defs_key ON sift.projection_defs (rel oid_ops, rel_name, name);
ALTER TABLE sift.projection_defs REPLICA IDENTITY USING INDEX projection_defs_key;
SELECT pg_catalog.pg_extension_config_dump('sift.projection_defs', 'WHERE rel::pg_catalog.oid IN (SELECT oid FROM pg_catalog.pg_class)');
-- the named projections of 5.0 name their table by its qualified name: a definition of a table that is gone is not dumped, as those of the tables above
SELECT pg_catalog.pg_extension_config_dump('sift.projections', 'WHERE pg_catalog.to_regclass(source) IS NOT NULL');

-- Skips the row of a table the database does not have: a restore without the table reads its name as 0 and has no rel_name to wait for it with, and the rows of two such tables that
-- share a projection name would meet in the unique key
CREATE OR REPLACE FUNCTION sift_internal.projection_defs_skip() RETURNS trigger
	LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	IF NEW.rel::pg_catalog.oid = 0 AND NEW.rel_name = '' THEN
		RETURN NULL;
	END IF;
	RETURN NEW;
END
$fn$;
COMMENT ON FUNCTION sift_internal.projection_defs_skip() IS 'Internal: BEFORE ROW trigger of sift.projection_defs, skips the row of a table that is not there and was not named (a restore without it)';
CREATE OR REPLACE TRIGGER projection_defs_skip BEFORE INSERT OR UPDATE ON sift.projection_defs
	FOR EACH ROW EXECUTE FUNCTION sift_internal.projection_defs_skip();

-- The names of the relations of the rows follow a rename, and the row of a sift_col index that a restore loaded before the index is bound to it when it is built. SHARE ROW EXCLUSIVE: a COPY of the rows
-- that is still running (pg_restore -j loads them beside the post-data items) finishes first, and one that starts later finds the index and reads its name as its OID. SHARE ROW EXCLUSIVE conflicts with itself: two of these do not both hold SHARE and wait for each other to raise it to ROW EXCLUSIVE.
CREATE OR REPLACE FUNCTION sift_internal.projection_defs_bind() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	IF EXISTS (SELECT 1 FROM pg_event_trigger_ddl_commands() d JOIN pg_class c ON c.oid = d.objid JOIN pg_am a ON a.oid = c.relam
			   WHERE d.command_tag = 'CREATE INDEX' AND a.amname = 'sift_col') THEN
		LOCK TABLE sift.projection_defs IN SHARE ROW EXCLUSIVE MODE;
		UPDATE sift.projection_defs v SET rel = c.oid::regclass::sift.tblref
		FROM pg_event_trigger_ddl_commands() d JOIN pg_class c ON c.oid = d.objid JOIN pg_namespace n ON n.oid = c.relnamespace
		WHERE d.command_tag = 'CREATE INDEX' AND v.rel::oid = 0 AND v.rel_name = quote_ident(n.nspname) || '.' || quote_ident(c.relname);
	END IF;
	UPDATE sift.projection_defs v SET rel_name = quote_ident(n.nspname) || '.' || quote_ident(c.relname)
	FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
	WHERE v.rel::oid = c.oid AND v.rel_name <> quote_ident(n.nspname) || '.' || quote_ident(c.relname);
END
$fn$;
COMMENT ON FUNCTION sift_internal.projection_defs_bind() IS 'Internal: after CREATE INDEX, ALTER INDEX, ALTER TABLE and ALTER SCHEMA, binds the rows of sift.projection_defs that wait for a sift_col index and keeps the names of the others';

DO $$ BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_event_trigger WHERE evtname = 'sift_projection_defs_bind') THEN
		CREATE EVENT TRIGGER sift_projection_defs_bind ON ddl_command_end WHEN TAG IN ('CREATE INDEX', 'ALTER INDEX', 'ALTER TABLE', 'ALTER SCHEMA') EXECUTE FUNCTION sift_internal.projection_defs_bind();
	END IF;
END $$;
COMMENT ON EVENT TRIGGER sift_projection_defs_bind IS 'After CREATE INDEX and a rename: binds the rows of sift.projection_defs of a sift_col index built by a restore, and keeps the names of the relations';

-- SECURITY DEFINER: the owner who drops a table has no right on sift.projection_defs
CREATE OR REPLACE FUNCTION sift_internal.projection_defs_drop() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	DELETE FROM sift.projection_defs v USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND v.rel::oid = d.objid;
END
$fn$;
COMMENT ON FUNCTION sift_internal.projection_defs_drop() IS 'Internal: after a DROP, removes the rows of sift.projection_defs of the tables that went';

DO $$ BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_event_trigger WHERE evtname = 'sift_projection_defs_drop') THEN
		CREATE EVENT TRIGGER sift_projection_defs_drop ON sql_drop EXECUTE FUNCTION sift_internal.projection_defs_drop();
	END IF;
END $$;
COMMENT ON EVENT TRIGGER sift_projection_defs_drop IS 'After a DROP: removes the rows of sift.projection_defs of the tables that went';

CREATE OR REPLACE FUNCTION sift.create_projection(rel regclass, name text, spec text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_create_projection' LANGUAGE C VOLATILE STRICT SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp;

CREATE OR REPLACE FUNCTION sift.drop_projection(rel regclass, name text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_drop_projection' LANGUAGE C VOLATILE STRICT SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp;

CREATE OR REPLACE FUNCTION sift.projection_list()
RETURNS TABLE (relation regclass, name text, kind text, state text, spec text, rows bigint, bytes bigint, synced_version bigint)
	AS 'MODULE_PATHNAME', 'sift_projection_list' LANGUAGE C STABLE SECURITY INVOKER SET search_path = pg_catalog, sift, pg_temp;

CREATE OR REPLACE VIEW sift.secondary_projections WITH (security_invoker = true) AS SELECT * FROM sift.projection_list();
GRANT SELECT ON sift.secondary_projections TO PUBLIC;

COMMENT ON FUNCTION sift.create_projection(regclass, text, text) IS 'Creates a projection of a sift index or a sift_table of format major 2 and builds it from the parts it has: spec is ORDER BY c1 [DESC], ... [INCLUDE (c, ...)] (a second sort order) or GROUP BY k1, ... MEASURES count(*), sum(m), ... (a pre-aggregate by base part). The planner chooses it when it is cheaper than the base parts. Needs MAINTAIN on the table. true when created.';
COMMENT ON FUNCTION sift.drop_projection(regclass, text) IS 'Drops the projection of the relation with the name, with the pages of its parts; false when there is none. Needs MAINTAIN on the table.';
COMMENT ON FUNCTION sift.projection_list() IS 'The projections of the relations the caller may read: state (building, ready, stale), the canonical text of the spec, rows and bytes of its parts and the manifest version of the commit that last wrote it.';
COMMENT ON VIEW sift.secondary_projections IS 'The projections of every sift index and sift_table of format major 2 the caller may read: relation, name, kind (sort, aggregate), state, spec, rows, bytes, synced_version.';
COMMENT ON TABLE sift.projection_defs IS 'The definitions of the projections (sift.create_projection writes a row, sift.drop_projection takes it off); dumped with the database.';
