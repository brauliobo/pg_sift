-- What is kept under the name of a table or of a column follows the name when it changes (ALTER TABLE .. RENAME COLUMN, RENAME TO, SET SCHEMA): the column lists in the options of the
-- sift_col indexes of the table and their attributes, the columns that sift.scope_follows names, and the names of the value stores of the table (docs/COL-VSTORE.md). An event trigger
-- and not the object access hook of the library: a session that renames and has not used sift loads no library, and the trigger does.
-- SECURITY DEFINER: the table owner who renames a column has no right to sift.scope_follows, whose columns the trigger keeps
CREATE FUNCTION sift.col_names_follow() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_col_names_follow' LANGUAGE C SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
CREATE EVENT TRIGGER sift_col_names_follow ON ddl_command_end WHEN TAG IN ('ALTER TABLE', 'ALTER MATERIALIZED VIEW') EXECUTE FUNCTION sift.col_names_follow();
COMMENT ON FUNCTION sift.col_names_follow() IS 'Internal: the event trigger after ALTER TABLE that makes what the indexes, sift.scope_follows and the value stores keep under the name of a renamed column or table, or of a table moved to another schema, follow it';
COMMENT ON EVENT TRIGGER sift_col_names_follow IS 'After ALTER TABLE RENAME COLUMN, RENAME TO or SET SCHEMA: what the indexes (options and attribute names), sift.scope_follows and the value stores keep under that name follows it';
