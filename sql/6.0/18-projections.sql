-- Secondary projections of a sift_col index or sift_table of Format 2.0: a second sort order of the rows, or a pre-aggregate by part, kept beside them and chosen by the planner when it answers
-- a query for less. They are not the named projections of 5.0 (sift.projections, sift.projection_create): that table keeps its name, and this part lists its own in sift.secondary_projections.

-- The definitions the projections of the directory are checked against: a projection with no definition, or one that no longer parses against the relation, is stale and never read.
-- The key of a relation is written as its qualified name, like the per-table options (sift.tblref).
CREATE TABLE IF NOT EXISTS sift.projection_defs (rel sift.tblref NOT NULL, name text NOT NULL, spec text NOT NULL);
CREATE UNIQUE INDEX IF NOT EXISTS projection_defs_key ON sift.projection_defs (rel oid_ops, name);
ALTER TABLE sift.projection_defs REPLICA IDENTITY USING INDEX projection_defs_key;
SELECT pg_catalog.pg_extension_config_dump('sift.projection_defs', 'WHERE rel::pg_catalog.oid IN (SELECT oid FROM pg_catalog.pg_class)');

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
	AS 'MODULE_PATHNAME', 'sift_create_projection' LANGUAGE C VOLATILE STRICT SECURITY INVOKER SET search_path = pg_catalog, sift;

CREATE OR REPLACE FUNCTION sift.drop_projection(rel regclass, name text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_drop_projection' LANGUAGE C VOLATILE STRICT SECURITY INVOKER SET search_path = pg_catalog, sift;

CREATE OR REPLACE FUNCTION sift.projection_list()
RETURNS TABLE (relation regclass, name text, kind text, state text, spec text, rows bigint, bytes bigint, synced_version bigint)
	AS 'MODULE_PATHNAME', 'sift_projection_list' LANGUAGE C STABLE SECURITY INVOKER SET search_path = pg_catalog, sift;

CREATE OR REPLACE VIEW sift.secondary_projections WITH (security_invoker = true) AS SELECT * FROM sift.projection_list();
GRANT SELECT ON sift.secondary_projections TO PUBLIC;

COMMENT ON FUNCTION sift.create_projection(regclass, text, text) IS 'Creates a projection of a sift index or a sift_table of format major 2 and builds it from the parts it has: spec is ORDER BY c1 [DESC] [NULLS FIRST], ... [INCLUDE (c, ...)] (a second sort order of the rows) or GROUP BY k1, date_trunc(''day'', t), ... MEASURES count(*), sum(m), avg(m), min(m), max(m) (a pre-aggregate by base part). The planner chooses it when it answers a query for less than the base parts, and EXPLAIN (ANALYZE) names it. Needs MAINTAIN on the table. true when created.';
COMMENT ON FUNCTION sift.drop_projection(regclass, text) IS 'Drops the projection of the relation with the name, with the pages of its parts; false when there is none. Needs MAINTAIN on the table.';
COMMENT ON FUNCTION sift.projection_list() IS 'The projections of the relations the caller may read: state (building, ready, stale), the canonical text of the spec, rows and bytes of its parts and the manifest version of the commit that last wrote it.';
COMMENT ON VIEW sift.secondary_projections IS 'The projections of every sift index and sift_table of format major 2 the caller may read: relation, name, kind (sort, aggregate), state, spec, rows, bytes, synced_version.';
COMMENT ON TABLE sift.projection_defs IS 'The definitions of the projections (sift.create_projection writes a row, sift.drop_projection takes it off); dumped with the database.';
