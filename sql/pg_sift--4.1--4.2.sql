\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.2'" to load this file. \quit

-- Per-table options of a sift_table (docs/COL-TAM.md section 8). A table access method has no reloptions before PostgreSQL 20, so the settings that act on one
-- table (sift.table_*) can be overridden for it with sift.table_set_option; the rows are in sift.table_option_values, keyed by the table.

-- The key of a table: its OID, written (by pg_dump, by COPY) as its qualified name and read back as the OID the table has in the database that reads it. A name
-- with no table reads as 0, and the row of 0 is skipped (the trigger below), so a restore that leaves a table out does not fail on its options. Binary compatible
-- with oid and regclass.
CREATE TYPE sift.tblref;

CREATE FUNCTION sift.tblref_in(cstring) RETURNS sift.tblref
	AS 'MODULE_PATHNAME', 'sift_tblref_in' LANGUAGE C STABLE STRICT;

CREATE FUNCTION sift.tblref_out(sift.tblref) RETURNS cstring
	AS 'MODULE_PATHNAME', 'sift_tblref_out' LANGUAGE C STABLE STRICT;

CREATE TYPE sift.tblref (INPUT = sift.tblref_in, OUTPUT = sift.tblref_out, INTERNALLENGTH = 4, PASSEDBYVALUE, ALIGNMENT = int4);

CREATE CAST (sift.tblref AS oid) WITHOUT FUNCTION AS IMPLICIT;
CREATE CAST (sift.tblref AS regclass) WITHOUT FUNCTION;
CREATE CAST (regclass AS sift.tblref) WITHOUT FUNCTION AS ASSIGNMENT;

CREATE TABLE sift.table_option_values (rel sift.tblref NOT NULL, name text NOT NULL, value text NOT NULL);
CREATE UNIQUE INDEX table_option_values_key ON sift.table_option_values (rel oid_ops, name);
-- dumped with the database; a row of a table that is gone (a stale OID prints as a number) is never written
SELECT pg_catalog.pg_extension_config_dump('sift.table_option_values', 'WHERE rel::pg_catalog.oid IN (SELECT oid FROM pg_catalog.pg_class)');

-- Skips the row of a table the database does not have, and replaces the entry of the table in the cache of every backend
CREATE FUNCTION sift.table_option_values_trg() RETURNS trigger
	AS 'MODULE_PATHNAME', 'sift_table_option_values_trg' LANGUAGE C;
CREATE TRIGGER table_option_values_trg BEFORE INSERT OR UPDATE OR DELETE ON sift.table_option_values
	FOR EACH ROW EXECUTE FUNCTION sift.table_option_values_trg();

-- The options go with the table.
CREATE FUNCTION sift.table_option_values_drop() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
BEGIN
	DELETE FROM sift.table_option_values v USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND v.rel::oid = d.objid;
END
$fn$;

CREATE EVENT TRIGGER sift_table_option_values_drop ON sql_drop EXECUTE FUNCTION sift.table_option_values_drop();

-- The names of the per-table options: the sift.table_* settings without the prefix
CREATE FUNCTION sift._table_option_names() RETURNS text[]
	AS 'MODULE_PATHNAME', 'sift_table_option_names' LANGUAGE C IMMUTABLE STRICT;

-- Sets the option of the sift_table tbl; the value is checked as the setting of the same name checks it (type, range, unit). The owner of the table.
CREATE FUNCTION sift.table_set_option(tbl regclass, name text, value text) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_table_set_option' LANGUAGE C VOLATILE;

-- Takes the option off the table (all its options when name is NULL): the setting applies again.
CREATE FUNCTION sift.table_reset_option(tbl regclass, name text DEFAULT NULL) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_table_reset_option' LANGUAGE C VOLATILE;

-- The value in force on the table: its own option, else the setting.
CREATE FUNCTION sift.table_get_option(tbl regclass, name text) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_table_get_option' LANGUAGE C STABLE STRICT;

-- The option as the table stores it; NULL when the table sets none.
CREATE FUNCTION sift._table_option_stored(tbl regclass, name text) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_table_option_stored' LANGUAGE C STABLE STRICT;

-- The options of the sift_tables the caller may see (or of tbl): the value in force, where it comes from (table: set on the table; setting: the sift.table_*
-- setting is not at its built-in default; default) and the built-in default.
CREATE FUNCTION sift.table_options(tbl regclass DEFAULT NULL)
	RETURNS TABLE (rel regclass, name text, value text, source text, default_value text)
	LANGUAGE plpgsql STABLE AS $fn$
BEGIN
	PERFORM sift._table_option_names();		-- loads the library, which defines the settings
	RETURN QUERY
	SELECT c.oid::regclass, o.name,
		   coalesce(sift._table_option_stored(c.oid, o.name), pg_catalog.current_setting('sift.table_' || o.name)),
		   CASE WHEN sift._table_option_stored(c.oid, o.name) IS NOT NULL THEN 'table' WHEN s.source <> 'default' THEN 'setting' ELSE 'default' END,
		   s.boot_val || coalesce(s.unit, '')
	FROM pg_catalog.pg_class c
	JOIN pg_catalog.pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL pg_catalog.unnest(sift._table_option_names()) WITH ORDINALITY AS o(name, ord)
	JOIN pg_catalog.pg_settings s ON s.name = 'sift.table_' || o.name
	WHERE c.relkind IN ('r', 'm') AND (tbl IS NULL OR c.oid = tbl)
	  AND pg_catalog.has_table_privilege(c.oid, 'SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
	ORDER BY c.oid::regclass::text, o.ord;
END
$fn$;

COMMENT ON TABLE sift.table_option_values IS 'The per-table options of the sift_tables, one row for each option a table sets (sift.table_set_option writes it, sift.table_options reads it); dumped with the database.';
COMMENT ON FUNCTION sift.table_set_option(regclass, text, text) IS 'Sets a per-table option of a sift_table (the name of a sift.table_* setting without the prefix); the value is checked as the setting checks it. The table option beats the setting whatever its source.';
COMMENT ON FUNCTION sift.table_reset_option(regclass, text) IS 'Takes an option (all of them when the name is NULL) off a sift_table, so that the setting applies again.';
COMMENT ON FUNCTION sift.table_get_option(regclass, text) IS 'The value of a per-table option in force on a sift_table: its own option, else the setting.';
COMMENT ON FUNCTION sift.table_options(regclass) IS 'The per-table options of the sift_tables the caller may see (or of one): value in force, source (table, setting, default) and built-in default.';
COMMENT ON FUNCTION sift._table_option_names() IS 'The names of the per-table options of a sift_table (the support of sift.table_options).';
COMMENT ON FUNCTION sift._table_option_stored(regclass, text) IS 'The option as the table stores it, NULL when the table sets none (the support of sift.table_options).';
COMMENT ON FUNCTION sift.table_option_values_trg() IS 'Trigger of sift.table_option_values: skips the row of a table the database does not have and replaces the entry of the table in the cache of every backend.';
COMMENT ON FUNCTION sift.tblref_in(cstring) IS 'Input of sift.tblref: the qualified name of a table, read as its OID here (0 for a table that is not there).';
COMMENT ON FUNCTION sift.tblref_out(sift.tblref) IS 'Output of sift.tblref: the qualified name of the table.';

-- What to change for each slug of sift.refusal_slugs() (the statement, the index or a setting: the HINT of the notice of sift.explain_refusals). A function of its own: the result of
-- refusal_slugs() cannot gain a column without dropping the function, and a view that reads it would fail the update.
CREATE FUNCTION sift.refusal_actions(OUT slug text, OUT action text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_refusal_actions' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.refusal_actions() IS 'What to change for each slug of sift.refusal_slugs(): the statement, the index or a setting (the hint of the notice of sift.explain_refusals); join it to sift.explain_why on slug';
COMMENT ON FUNCTION sift.help(text) IS 'The row of docs/messages.tsv of a pg_sift message or choice by its slug, or the registry row of a refusal slug of sift.explain_why (all message rows without an argument)';

-- sift.workers also lists the sift_table: its column is named index and holds the table, kind is sift_table, and its segments and sealed write buffers are as for an index.
-- The background of a table keeps no note of a failed step, so its failures, failed_at, next_retry, error_code and last_error are 0 and NULL, and due is NULL.
CREATE OR REPLACE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind = 'i' AND has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error, NULL::text
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind IN ('r', 'm') AND has_table_privilege(c.oid, 'SELECT') AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index and per sift_table whose table you can read (index: the index, or the table): segments, sealed write buffers, whether maintenance runs, the failures in a row of the background, when, the next retry, the SQLSTATE and the last error (a sift_table keeps none), and why a sift_col index would be rewritten (due).';
