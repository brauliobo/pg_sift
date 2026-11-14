-- sift.build_info(): what the loaded library was built with (version, PostgreSQL, the lz4 and zstd it links or "no", assertions), to compare two servers.
CREATE FUNCTION sift.build_info() RETURNS TABLE (name text, value text)
	AS 'MODULE_PATHNAME', 'sift_build_info' LANGUAGE C STABLE;
COMMENT ON FUNCTION sift.build_info() IS 'What the loaded library was built with: version, PostgreSQL, the lz4 and zstd it links (no when left out), assertions';

-- The ALTER DATABASE command that preloads the library for the database, or NULL when shared_preload_libraries or session_preload_libraries names pg_sift already.
-- A library is named with or without a directory, quotes or extension ('$libdir/pg_sift', "pg_sift.so"); the value this session has now is kept in the command.
CREATE FUNCTION sift._preload_hint() RETURNS text LANGUAGE sql STABLE SET search_path = pg_catalog AS $$
	SELECT CASE WHEN EXISTS (
			SELECT 1 FROM unnest(string_to_array(current_setting('shared_preload_libraries') || ',' || current_setting('session_preload_libraries'), ',')) AS l
			WHERE regexp_replace(regexp_replace(btrim(l, E' \t"'), '^.*/', ''), '[.](so|dylib|dll)$', '') = 'pg_sift')
		THEN NULL
		ELSE format('ALTER DATABASE %I SET session_preload_libraries = %L;', current_database(),
			CASE WHEN current_setting('session_preload_libraries') = '' THEN 'pg_sift' ELSE current_setting('session_preload_libraries') || ', pg_sift' END)
	END
$$;

-- The end of CREATE EXTENSION and ALTER EXTENSION UPDATE: the first statement of a session that loads the library is planned twice unless it is preloaded (docs/USAGE.md).
-- PostgreSQL runs an extension script with client_min_messages raised to warning, which would hide a notice: it is lowered for the one RAISE and put back.
-- pg_sift.install_notice = off silences the notice; the clusters of the test harness set it (scripts/pgtest.sh conf), so that no expected output carries it.
DO $$
DECLARE
	hint text := sift._preload_hint();
	level text := current_setting('client_min_messages');
BEGIN
	IF hint IS NOT NULL AND coalesce(current_setting('pg_sift.install_notice', true), 'on') <> 'off' THEN
		IF level = 'warning' THEN PERFORM set_config('client_min_messages', 'notice', true); END IF;
		RAISE NOTICE 'pg_sift is not preloaded in database "%": the first statement of each session is planned twice', current_database() USING HINT = hint;
		PERFORM set_config('client_min_messages', level, true);
	END IF;
END
$$;
