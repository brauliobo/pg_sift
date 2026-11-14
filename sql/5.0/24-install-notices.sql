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

-- The end of CREATE EXTENSION and ALTER EXTENSION UPDATE: one notice with what is left to do. The first statement of a session that loads the library is planned twice
-- unless it is preloaded, and SHOW sift.<setting> fails until a session has loaded it (docs/USAGE.md); the sift indexes and tables the database holds are listed by
-- sift.upgrade_commands(). Nothing is said when the library is preloaded and the database holds none.
-- PostgreSQL runs an extension script with client_min_messages raised to warning, which would hide a notice: it is lowered for the one RAISE and put back.
-- pg_sift.install_notice = off silences the notice; the clusters of the test harness set it (scripts/pgtest.sh conf), so that no expected output carries it.
DO $$
DECLARE
	hint   text := sift._preload_hint();
	level  text := current_setting('client_min_messages');
	n      int  := (SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col', 'sift_table'));
	listed text := CASE WHEN n > 0 THEN format('%s sift indexes and tables in this database: SELECT * FROM sift.upgrade_commands() lists what is left to do for them. The column formats that 4.3 and older refuse are written only by the features that need them (docs/UPGRADING.md, what 5.0 writes that 4.3 does not read).', n) END;
BEGIN
	IF coalesce(current_setting('pg_sift.install_notice', true), 'on') = 'off' OR (hint IS NULL AND n = 0) THEN RETURN; END IF;
	IF level = 'warning' THEN PERFORM set_config('client_min_messages', 'notice', true); END IF;
	IF hint IS NOT NULL THEN
		RAISE NOTICE 'pg_sift is not preloaded in database "%": the first statement of each session is planned twice', current_database()
			USING DETAIL = concat_ws(' ', 'SHOW sift.<setting> says unrecognized configuration parameter in a session until the library is loaded.', listed), HINT = hint;
	ELSE
		RAISE NOTICE 'pg_sift is installed in database "%"', current_database() USING DETAIL = listed;
	END IF;
	PERFORM set_config('client_min_messages', level, true);
END
$$;
