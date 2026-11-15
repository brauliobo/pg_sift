-- What is kept under the name of a table, an index or a column follows the name when it changes (ALTER TABLE .. RENAME COLUMN, RENAME TO, SET SCHEMA; ALTER INDEX .. RENAME TO): the column
-- lists, connect, scope and paths in the options of the sift_col indexes and their attributes, the columns that sift.scope_follows names, the definitions of sift.projections, the paths
-- and options of a sift_table, and the names of the value stores of the table (docs/COL-VSTORE.md). An event trigger and not the object access hook of the library: a session that
-- renames and has not used sift loads no library, and the trigger does.
-- SECURITY DEFINER: the table owner who renames a column has no right to sift.scope_follows, whose columns the trigger keeps
CREATE FUNCTION sift.col_names_follow() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_col_names_follow' LANGUAGE C SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
-- SET SCHEMA does not tell the schema the table had: this notes it before
CREATE FUNCTION sift.col_names_before() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_col_names_before' LANGUAGE C;
CREATE EVENT TRIGGER sift_col_names_before ON ddl_command_start WHEN TAG IN ('ALTER TABLE', 'ALTER MATERIALIZED VIEW') EXECUTE FUNCTION sift.col_names_before();
CREATE EVENT TRIGGER sift_col_names_follow ON ddl_command_end WHEN TAG IN ('ALTER TABLE', 'ALTER MATERIALIZED VIEW', 'ALTER INDEX') EXECUTE FUNCTION sift.col_names_follow();
COMMENT ON FUNCTION sift.col_names_follow() IS 'Internal: the event trigger after ALTER TABLE and ALTER INDEX that makes what the indexes, sift.scope_follows, sift.projections and the value stores keep under the name of a renamed column, table or index, or of a table moved to another schema, follow it';
COMMENT ON FUNCTION sift.col_names_before() IS 'Internal: the event trigger before ALTER TABLE that notes the schema and the name a table has before SET SCHEMA';
COMMENT ON EVENT TRIGGER sift_col_names_follow IS 'After ALTER TABLE RENAME COLUMN, RENAME TO or SET SCHEMA and ALTER INDEX RENAME TO: what the indexes (options and attribute names), sift.scope_follows, sift.projections and the value stores keep under that name follows it';
COMMENT ON EVENT TRIGGER sift_col_names_before IS 'Before ALTER TABLE: notes the schema and the name of a table that SET SCHEMA is about to move';

-- The names of the definitions of sift.projections and of the options of a sift_table, written with a column renamed: the functions of the event trigger
CREATE FUNCTION sift._names_following() RETURNS boolean AS 'MODULE_PATHNAME', 'sift_names_following' LANGUAGE C STABLE PARALLEL RESTRICTED;
CREATE FUNCTION sift._names_expr(expression text, from_name text, to_name text) RETURNS text AS 'MODULE_PATHNAME', 'sift_names_expr' LANGUAGE C IMMUTABLE;
CREATE FUNCTION sift._names_exprs(expressions text[], from_name text, to_name text) RETURNS text[] AS 'MODULE_PATHNAME', 'sift_names_exprs' LANGUAGE C IMMUTABLE;
CREATE FUNCTION sift._names_option(option text, value text, from_name text, to_name text) RETURNS text AS 'MODULE_PATHNAME', 'sift_names_option' LANGUAGE C IMMUTABLE;
COMMENT ON FUNCTION sift._names_following() IS 'Internal: whether sift.col_names_follow is at work; the guards of sift.projections and sift.scope_follows let its changes through';
COMMENT ON FUNCTION sift._names_expr(text, text, text) IS 'Internal: an expression with the references to the column from_name named to_name';
COMMENT ON FUNCTION sift._names_exprs(text[], text, text) IS 'Internal: each expression of the array with the references to the column from_name named to_name';
COMMENT ON FUNCTION sift._names_option(text, text, text, text) IS 'Internal: the value of a sift_col option that lists columns, with the column from_name named to_name';

-- The rows for tables, views and schemas that are dropped go with them: a follow whose source, pair table or view is gone (the triggers of a source that is left are dropped), and the
-- definitions of the projections of a table (an index that listed one went with the table). DROP INDEX loads no library: the trigger runs as the owner of the extension, the role that drops has no right on the sift schema.
CREATE FUNCTION sift._col_names_sql_drop() RETURNS event_trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	f sift.scope_follows;
	t text;
BEGIN
	FOR f IN DELETE FROM sift.scope_follows s USING pg_event_trigger_dropped_objects() d
			 WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND d.objid IN (s.pairs::oid, s.definition::oid, s.source::oid) RETURNING s.* LOOP
		IF f.source::oid IN (SELECT c.oid FROM pg_class c) THEN
			FOREACH t IN ARRAY ARRAY['sins', 'supd', 'sdel', 'st'] LOOP
				EXECUTE format('DROP TRIGGER IF EXISTS %I ON %s', f.trigger_base || '_' || t, f.source);
			END LOOP;
		END IF;
	END LOOP;
	DELETE FROM sift.projections p USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND d.schema_name IS NOT NULL AND p.source = format('%I.%I', d.schema_name, d.object_name);
END $$;
CREATE EVENT TRIGGER sift_col_names_sql_drop ON sql_drop EXECUTE FUNCTION sift._col_names_sql_drop();
COMMENT ON FUNCTION sift._col_names_sql_drop() IS 'Internal: after a DROP, removes the rows of sift.scope_follows and sift.projections of the tables and views that went';
COMMENT ON EVENT TRIGGER sift_col_names_sql_drop IS 'After a DROP: removes the rows of sift.scope_follows and sift.projections of the tables and views that went';
