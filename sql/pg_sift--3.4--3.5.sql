\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.5'" to load this file. \quit

-- 3.5 adds one SQL object, sift.json_build_object (below); the library is new (the planner, the column format minor 22). The update also moves the version, so that
-- sift.version() and the catalog agree and ALTER EXTENSION UPDATE is the one step of every upgrade.

-- json_build_object with the type lookups of each argument made once per call site, not per call; the same text (test json_build_object). Put sift before pg_catalog in the search_path to take it for every call of the name.
CREATE FUNCTION sift.json_build_object(VARIADIC "any") RETURNS json
	AS 'MODULE_PATHNAME', 'sift_json_build_object' LANGUAGE C STABLE PARALLEL SAFE;
COMMENT ON FUNCTION sift.json_build_object("any") IS 'json_build_object with the type lookups of each argument made once per call site: the same text as the core function, for one or more arguments';

-- order_array_columns is chosen by the build in 3.5 (auto) and can be off: neither is a column to reindex for (the pattern of the update list took any value)
CREATE OR REPLACE FUNCTION sift.upgrade_commands(OUT kind text, OUT object regclass, OUT reason text, OUT command text, OUT required bool)
	RETURNS SETOF record
	LANGUAGE plpgsql SET lock_timeout = '100ms' AS $fn$
DECLARE
	r record;
	lo int;
	why text;
BEGIN
	kind := 'reindex'; required := true;
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift' AND c.relkind = 'i' AND has_table_privilege(i.indrelid, 'MAINTAIN') AND 'analyzer=keyword' <> ALL (coalesce(c.reloptions, '{}')) ORDER BY n.nspname, c.relname
	LOOP
		object := r.relid; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		reason := 'long words (over 255 bytes) are found only after REINDEX: an index built before 3.3 holds such a word cut at 255 bytes, so a query for it finds nothing';
		RETURN NEXT;
	END LOOP;
	required := false;
	FOR r IN SELECT c.oid AS relid, c.reloptions AS opts, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift_col' AND c.relkind = 'i' AND has_table_privilege(i.indrelid, 'MAINTAIN') ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min((to_jsonb(s)->>'format')::int) INTO lo FROM sift.col_segments(r.relid) s;
		EXCEPTION WHEN OTHERS THEN
			IF SQLSTATE IN ('42883', '58P01', '55P03') THEN RAISE; END IF;
			CONTINUE;
		END;
		CONTINUE WHEN lo IS NULL;
		-- a segment is minor 9, 10, 13, 14, 15 or 20 when some unit holds the section (for 14, when the segment holds a point directory), so a lower minor means none does (docs/COL.md 2.13, 2.14, 2.17)
		-- cube_minmax and superunit are on unless the index says otherwise (dflt): the units written before 3.3 lack their sections
		SELECT string_agg(format('%s with column format %s: %s', v.what, lo, v.why), '; ' ORDER BY v.minor) INTO why FROM (VALUES
				('^paths=', 6, 'JSON paths', false,
				 'segments before format 6 may lack the kinds of a JSON null or of a type; a predicate that needs them uses the normal plan, and a REINDEX writes them'),
				('^cube_minmax=(false|off)$', 7, 'cube_minmax', true,
				 'the units written before the option scan; a REINDEX writes the minimum and maximum of every unit'),
				('^rollup_dims=.', 9, 'rollup_dims', false,
				 'the units written before the option have no rollup of their rows and are scanned (nor has a unit over the caps of the rollup, which a REINDEX does not change); a REINDEX writes it where it fits'),
				('^key_columns=.', 10, 'key_columns', false,
				 'the units written before the option have no keys of the column (a summary, or a key list for a count(DISTINCT)), so an equality or a distinct count reads them whole (nor has a unit whose keys would take more than 3% of its bytes, which a REINDEX does not change); a REINDEX writes them where they fit'),
				('^superunit=0$', 12, 'superunit', true,
				 'the units written before the option have no summary of a group of units, so a scan of the whole index reads and folds every unit; a REINDEX writes it'),
				('^order_columns=.', 13, 'order_columns', false,
				 'the units written before the option have no sorted run, so an ORDER BY .. LIMIT on the column reads them whole; a REINDEX writes it'),
				('^point_columns=.', 14, 'point_columns', false,
				 'the segments written before the option have no point directory, so an equality or IN list on the column reads their units (zones and summaries cut most); a REINDEX writes it'),
				('^point_tid_columns=.', 16, 'point_tid_columns', false,
				 'the segments written before the option list the rows of the column by their number in the segment, so an equality or IN list on the column reads the units of the rows it finds; a REINDEX lists their heap addresses'),
				('^key_cut=(?!off$|auto$)', 17, 'key_cut', false,
				 'the units written before the option are cut by superblock, so a lookup or a range of the key reads units from every part of the heap; a REINDEX cuts them by the key'),
				('^hll_columns=.', 20, 'hll_columns', false,
				 'the units written before the option have no sketch of the column, so sift.approx_count_distinct reads their rows; a REINDEX writes it'),
				('^order_array_columns=(?!off$|auto$)', 21, 'order_array_columns', false,
				 'the segments written before the option have no global order of the columns, so an ORDER BY .. LIMIT above 32 rows on one reads their units; a REINDEX writes it')
			 ) v(pat, minor, what, dflt, why)
			 WHERE lo < v.minor AND EXISTS (SELECT 1 FROM unnest(r.opts) opt WHERE opt ~ v.pat) <> v.dflt;
		CONTINUE WHEN why IS NULL;
		object := r.relid; reason := why; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		RETURN NEXT;
	END LOOP;
	kind := 'rewrite';
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name,
					EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
							AND a.atttypid IN ('date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype)) AS calendar,
					EXISTS (SELECT 1 FROM sift.table_paths_maintained p WHERE p.relid = c.oid) AS paths
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace
			 WHERE am.amname = 'sift_table' AND c.relkind IN ('r', 'm') AND has_table_privilege(c.oid, 'MAINTAIN') ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min(s.format) INTO lo FROM sift.table_segments(r.relid) s WHERE s.kind = 'columns';
		EXCEPTION WHEN OTHERS THEN
			IF SQLSTATE IN ('42883', '58P01', '55P03') THEN RAISE; END IF;
			CONTINUE;
		END;
		-- the segments of a table with declared paths are never minor 8 (src/col/seg_write.c)
		why := concat_ws('; ',
				CASE WHEN r.calendar AND NOT r.paths AND lo < 8 THEN
					format('column format %s: the segments have no calendar cells of the date and timestamp columns, so a GROUP BY on a day or a month of them reads the rows', lo) END,
				CASE WHEN r.paths AND lo < 6 THEN
					format('column format %s: the segments may lack the kinds of a JSON null or of a type of the declared paths, and a predicate that needs them uses the normal plan', lo) END);
		CONTINUE WHEN why = '';
		object := r.relid; reason := why || '. It takes an ACCESS EXCLUSIVE lock and rewrites the table in the format of sift.col_format_minor: schedule it';
		command := format('VACUUM FULL %s;', r.name);
		RETURN NEXT;
	END LOOP;
END
$fn$;
