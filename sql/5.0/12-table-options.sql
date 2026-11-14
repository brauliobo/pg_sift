
-- sift.table_options listed an option only where a sift.table_* setting of its name exists. key_order has none: it is an option of a table and has no value
-- anywhere else, so its row says no value and no setting (the setting that stood for it held no value and refused every one).
CREATE OR REPLACE FUNCTION sift.table_options(tbl regclass DEFAULT NULL)
	RETURNS TABLE (rel regclass, name text, value text, source text, default_value text)
	LANGUAGE plpgsql STABLE AS $fn$
BEGIN
	PERFORM sift._table_option_names();		-- loads the library, which defines the settings
	RETURN QUERY
	SELECT c.oid::regclass, o.name,
		   coalesce(sift._table_option_stored(c.oid, o.name), pg_catalog.current_setting('sift.table_' || o.name, true), ''),
		   CASE WHEN sift._table_option_stored(c.oid, o.name) IS NOT NULL THEN 'table' WHEN s.source <> 'default' THEN 'setting' ELSE 'default' END,
		   coalesce(s.boot_val || coalesce(s.unit, ''), '')
	FROM pg_catalog.pg_class c
	JOIN pg_catalog.pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL pg_catalog.unnest(sift._table_option_names()) WITH ORDINALITY AS o(name, ord)
	LEFT JOIN pg_catalog.pg_settings s ON s.name = 'sift.table_' || o.name
	WHERE c.relkind IN ('r', 'm') AND (tbl IS NULL OR c.oid = tbl)
	  AND pg_catalog.has_table_privilege(c.oid, 'SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
	ORDER BY c.oid::regclass::text, o.ord;
END
$fn$;
