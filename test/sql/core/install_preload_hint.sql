-- The notice at the end of CREATE EXTENSION and ALTER EXTENSION UPDATE says that the library is not preloaded and gives the command that preloads it for the
-- database: sift._preload_hint() is that command, NULL once shared_preload_libraries or session_preload_libraries names pg_sift (in any spelling of a library
-- name, and not for another library whose name starts with it). The database name differs between runs, so it is replaced.
CREATE EXTENSION pg_sift;
CREATE FUNCTION hint() RETURNS text LANGUAGE sql AS $$ SELECT replace(sift._preload_hint(), current_database(), 'db') $$;

SELECT hint();
SELECT current_setting('session_preload_libraries') AS session, current_setting('shared_preload_libraries') AS shared;

-- table-driven over the spellings; each value is set for the session and read back
CREATE TEMP TABLE spellings (n serial, value text);
INSERT INTO spellings (value) VALUES ('pg_sift'), ('$libdir/pg_sift'), ('"pg_sift"'), ('pg_sift.so'), ('auto_explain, pg_sift'), ('/opt/lib/pg_sift.so ,auto_explain'),
	('auto_explain'), ('pg_sift_helpers'), ('my_pg_sift');
SELECT set_config('session_preload_libraries', value, false) AS set, hint() FROM spellings ORDER BY n;

-- the command keeps what the session has now
SET session_preload_libraries = 'auto_explain';
SELECT hint();
RESET session_preload_libraries;

-- sift.build_info(): the library of this build, its PostgreSQL and the codecs; two servers are compared with it
SELECT name FROM sift.build_info() ORDER BY name;
SELECT value = sift.version() AS same_version FROM sift.build_info() WHERE name = 'version';
SELECT value <> 'no' AS linked FROM sift.build_info() WHERE name IN ('lz4', 'zstd') ORDER BY name;

RESET session_preload_libraries;
DROP TABLE spellings;
DROP FUNCTION hint();
DROP EXTENSION pg_sift;
