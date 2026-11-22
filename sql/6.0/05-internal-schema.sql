-- The helpers that 6.0 adds for its own functions live in sift_internal, away from the public schema; sift.* is the documented surface. The internal functions that 5.1
-- created in sift (sift._check, sift.projection_plan, ...) stay where they are: the C catalog lookups and the objects that depend on them name them there.
CREATE SCHEMA IF NOT EXISTS sift_internal;
COMMENT ON SCHEMA sift_internal IS 'Internal helpers of pg_sift: not a public interface, they change between releases';
REVOKE ALL ON SCHEMA sift_internal FROM PUBLIC;
GRANT USAGE ON SCHEMA sift_internal TO PUBLIC;

-- The calm wording for a relation whose on-disk format is Format Major 1: fully supported, upgrading is optional.
CREATE OR REPLACE FUNCTION sift_internal.format_note(am text, major int, size_bytes int8) RETURNS text
	LANGUAGE sql IMMUTABLE PARALLEL SAFE SECURITY INVOKER SET search_path = pg_catalog, sift AS $$
	SELECT CASE WHEN size_bytes = 0 OR major IS NULL THEN 'empty'
				WHEN major >= 2 THEN 'Format 2.0'
				WHEN am = 'sift' THEN 'Format 1.7 (current)'
				WHEN am = 'sift_table' THEN 'Format 1.x (fully supported; optional SELECT * FROM sift.upgrade_table(...) upgrades to Format 2.0)'
				ELSE 'Format 1.x (fully supported; optional REINDEX INDEX CONCURRENTLY upgrades to Format 2.0)' END
$$;
COMMENT ON FUNCTION sift_internal.format_note(text, int, int8) IS 'Internal: the one-line note of sift.status() and sift.indexes on the format of a relation';

-- What sift.verify, sift.col_verify and sift.table_verify did: an error on the first problem sift.check finds.
CREATE OR REPLACE FUNCTION sift_internal.check_raise(rel regclass, heapallindexed bool) RETURNS void
	LANGUAGE plpgsql VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift AS $fn$
DECLARE
	p record;
BEGIN
	FOR p IN SELECT c.problem, c.detail FROM sift.check(rel, heapallindexed, 1) c LOOP
		RAISE EXCEPTION '%: %', p.problem, p.detail USING ERRCODE = 'data_corrupted', HINT = 'sift.check lists every problem of the relation';
	END LOOP;
END
$fn$;
COMMENT ON FUNCTION sift_internal.check_raise(regclass, bool) IS 'Internal: raises the first problem of sift.check, for the deprecated verify functions';

-- The ALTER DATABASE command that preloads the library for the database, or NULL when the library is preloaded already or the caller may not read the two settings
-- (they are for pg_read_all_settings: a monitoring role must get a health report, not an error, so it is told nothing about this).
CREATE OR REPLACE FUNCTION sift_internal.preload_hint() RETURNS text
	LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog, sift AS $$
	SELECT CASE WHEN count(*) = 2 AND count(s.setting) = 2
				 AND NOT EXISTS (SELECT 1 FROM unnest(string_to_array(string_agg(s.setting, ','), ',')) l
								 WHERE regexp_replace(regexp_replace(btrim(l, E' \t"'), '^.*/', ''), '[.](so|dylib|dll)$', '') = 'pg_sift')
				THEN format('ALTER DATABASE %I SET session_preload_libraries = %L;', current_database(),
							CASE WHEN max(s.setting) FILTER (WHERE s.name = 'session_preload_libraries') = '' THEN 'pg_sift'
								 ELSE max(s.setting) FILTER (WHERE s.name = 'session_preload_libraries') || ', pg_sift' END) END
	FROM pg_settings s WHERE s.name IN ('shared_preload_libraries', 'session_preload_libraries')
$$;
COMMENT ON FUNCTION sift_internal.preload_hint() IS 'Internal: the command that preloads the library for the database, NULL when it is preloaded or the settings are not readable';

-- The relation kind a deprecated function of 5.1 serves: any other relation gets the error it got in 5.1. want is sift_col, sift_table or sift (a sift or a sift_col index).
CREATE OR REPLACE FUNCTION sift_internal.require_am(rel regclass, want text) RETURNS void
	LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path = pg_catalog AS $fn$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_class c JOIN pg_am a ON a.oid = c.relam
				   WHERE c.oid = rel AND a.amname = ANY (CASE want WHEN 'sift' THEN ARRAY['sift', 'sift_col'] ELSE ARRAY[want] END)) THEN
		RAISE EXCEPTION '"%" is not a %', (SELECT relname FROM pg_class WHERE oid = rel), CASE want WHEN 'sift_table' THEN want ELSE want || ' index' END
			USING ERRCODE = 'wrong_object_type';
	END IF;
END
$fn$;
COMMENT ON FUNCTION sift_internal.require_am(regclass, text) IS 'Internal: raises the error of 5.1 unless the relation is of the access method the deprecated function serves (sift_col, sift_table, or sift for a sift or sift_col index)';

-- What a segment of Format 1 knows and sift.parts does not list: its dirty flag and its own column format. The deprecated sift.col_segments and sift.table_segments read a relation of
-- Format 1 through these (the entry points of 5.1) and a relation of Format 2.0 through sift.parts.
CREATE OR REPLACE FUNCTION sift_internal.v1_col_segments(index regclass, OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4,
														 OUT units int4, OUT dirty bool, OUT dead_rows int8, OUT extents int4, OUT format int4, OUT preagg_bytes int8)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_col_segments' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift_internal.v1_col_segments(regclass) IS 'Internal: the segments of a sift_col index of Format 1, with their dirty flag and column format';

CREATE OR REPLACE FUNCTION sift_internal.v1_table_segments(tbl regclass, OUT gen int8, OUT kind text, OUT rows int8, OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4,
														   OUT units int4, OUT dead_rows int8, OUT format int4, OUT unfrozen bool, OUT min_xid xid, OUT max_xid xid)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_table_segments' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift_internal.v1_table_segments(regclass) IS 'Internal: the segments of a sift_table of Format 1, with their column format';
