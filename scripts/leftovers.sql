-- One line per object in the current database that the cluster's template does not have and that no extension owns: what a regression test
-- left behind. Run it with psql -X -qAt as a superuser (scripts/pgtest.sh leftovers); -v cluster=on adds the objects the cluster itself holds
-- (roles, ALTER SYSTEM), which other jobs on the same cluster change too.
-- Built-in objects have an oid below 16384, which is also why plpgsql is not listed.
WITH o(classid, oid) AS (
	-- the indexes of the extension's own tables are the extension's
	SELECT 'pg_class'::regclass, c.oid FROM pg_class c WHERE c.relkind <> 't' AND c.relnamespace <> 'pg_toast'::regnamespace
		AND NOT EXISTS (SELECT 1 FROM pg_index i JOIN pg_depend d ON d.classid = 'pg_class'::regclass AND d.objid = i.indrelid AND d.deptype = 'e'
				WHERE i.indexrelid = c.oid)
	UNION ALL SELECT 'pg_proc'::regclass, oid FROM pg_proc
	-- the row types of relations and the array types come and go with their element
	UNION ALL SELECT 'pg_type'::regclass, oid FROM pg_type WHERE typrelid = 0 AND typcategory <> 'A' AND typtype <> 'm'
	-- CREATE EXTENSION makes the schemas sift and sift_helpers and DROP EXTENSION keeps them; a test empties public with DROP SCHEMA public CASCADE; CREATE SCHEMA public,
	-- which gives it an oid past the built-in ones
	UNION ALL SELECT 'pg_namespace'::regclass, oid FROM pg_namespace WHERE nspname NOT IN ('sift', 'sift_helpers', 'public') AND nspname !~ '^pg_(toast_)?temp_'
	UNION ALL SELECT 'pg_extension'::regclass, oid FROM pg_extension
	UNION ALL SELECT 'pg_operator'::regclass, oid FROM pg_operator
	UNION ALL SELECT 'pg_cast'::regclass, oid FROM pg_cast
	UNION ALL SELECT 'pg_collation'::regclass, oid FROM pg_collation
	UNION ALL SELECT 'pg_conversion'::regclass, oid FROM pg_conversion
	UNION ALL SELECT 'pg_opclass'::regclass, oid FROM pg_opclass
	UNION ALL SELECT 'pg_opfamily'::regclass, oid FROM pg_opfamily
	UNION ALL SELECT 'pg_statistic_ext'::regclass, oid FROM pg_statistic_ext
	UNION ALL SELECT 'pg_event_trigger'::regclass, oid FROM pg_event_trigger
	UNION ALL SELECT 'pg_publication'::regclass, oid FROM pg_publication
	UNION ALL SELECT 'pg_language'::regclass, oid FROM pg_language
	UNION ALL SELECT 'pg_foreign_data_wrapper'::regclass, oid FROM pg_foreign_data_wrapper
	UNION ALL SELECT 'pg_foreign_server'::regclass, oid FROM pg_foreign_server
	UNION ALL SELECT 'pg_ts_config'::regclass, oid FROM pg_ts_config
	UNION ALL SELECT 'pg_ts_dict'::regclass, oid FROM pg_ts_dict
	UNION ALL SELECT 'pg_largeobject_metadata'::regclass, oid FROM pg_largeobject_metadata
	UNION ALL SELECT 'pg_authid'::regclass, oid FROM pg_authid WHERE :'cluster' = 'on'
)
SELECT pg_describe_object(classid, oid, 0) FROM o
WHERE oid >= 16384 AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = o.classid AND d.objid = o.oid AND d.deptype = 'e')
UNION ALL
-- what pg_regress sets on the database it creates is not a leftover
SELECT 'database setting ' || s FROM pg_db_role_setting, unnest(setconfig) s
WHERE setdatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
  AND s !~ '^(lc_messages|lc_monetary|lc_numeric|lc_time|bytea_output|timezone_abbreviations)='
UNION ALL
SELECT 'ALTER SYSTEM ' || name || ' = ' || setting FROM pg_file_settings WHERE :'cluster' = 'on' AND sourcefile ~ 'auto\.conf$'
ORDER BY 1;
