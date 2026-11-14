
-- 5.0 removes key_cut_rows, point_columns and order_columns: a build ignores them, so a REINDEX no longer writes the point directory of point_columns (its row is gone) and
-- the command that an index holding one waits for is the RESET that drops it (kind reset). A relation that another session holds locked at once (lock_timeout) is a row of
-- kind skipped, not an error that hides the rest. A text index is listed until the REINDEX of a 5.0 build marks it (sift.index_format long_words). The rest is the function of 4.3.
CREATE OR REPLACE FUNCTION sift.upgrade_commands(OUT kind text, OUT object regclass, OUT reason text, OUT command text, OUT required bool)
	RETURNS SETOF record
	LANGUAGE plpgsql SET lock_timeout = '100ms' AS $fn$
DECLARE
	r record;
	lo int;
	why text;
	built boolean;
BEGIN
	kind := 'reindex'; required := true;
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift' AND c.relkind = 'i' AND i.indisvalid AND has_table_privilege(i.indrelid, 'MAINTAIN') AND 'analyzer=keyword' <> ALL (coalesce(c.reloptions, '{}')) ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT f.long_words INTO built FROM sift.index_format(r.relid) f;
		EXCEPTION
			WHEN lock_not_available THEN
				kind := 'skipped'; required := NULL; object := r.relid; reason := 'locked, run again'; command := NULL;
				RETURN NEXT;
				kind := 'reindex'; required := true;
				CONTINUE;
			WHEN OTHERS THEN
				IF SQLSTATE IN ('42883', '58P01') THEN RAISE; END IF;
				CONTINUE;
		END;
		CONTINUE WHEN built;
		object := r.relid; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		reason := 'long words (over 255 bytes) may be found only after REINDEX: the index was built by a release before 5.0, which does not record whether it holds such a word cut at 255 bytes (an index built before 3.3 does), so a query for it finds nothing; the REINDEX marks the index and takes it off this list';
		RETURN NEXT;
	END LOOP;
	required := false;
	FOR r IN SELECT c.oid AS relid, c.reloptions AS opts, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift_col' AND c.relkind = 'i' AND has_table_privilege(i.indrelid, 'MAINTAIN') ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min((to_jsonb(s)->>'format')::int) INTO lo FROM sift.col_segments(r.relid) s;
		EXCEPTION
			WHEN lock_not_available THEN
				kind := 'skipped'; required := NULL; object := r.relid; reason := 'locked, run again'; command := NULL;
				RETURN NEXT;
				kind := 'reindex'; required := false;
				CONTINUE;
			WHEN OTHERS THEN
				IF SQLSTATE IN ('42883', '58P01') THEN RAISE; END IF;
				CONTINUE;
		END;
		CONTINUE WHEN lo IS NULL;
		-- a segment is minor 9, 10, 13, 14, 15 or 20 when some unit holds the section (for 14, when the segment holds a point directory), so a lower minor means none does
		-- cube_minmax and superunit are on unless the index says otherwise (dflt): the units written before 3.3 lack their sections
		SELECT string_agg(format('%s with column format %s: %s', v.what, lo, v.why), '; ' ORDER BY v.minor) INTO why FROM (VALUES
				('^paths=', 6, 'JSON paths', false,
				 'segments before format 6 may lack the kinds of a JSON null or of a type; a predicate that needs them uses the normal plan, and a REINDEX writes them'),
				('^cube_minmax=(false|off)$', 7, 'cube_minmax', true,
				 'the units written before the option scan; a REINDEX writes the minimum and maximum of every unit'),
				('^(rollup_dims|rollup_columns)=.', 9, 'rollup_columns', false,
				 'the units written before the option have no rollup of their rows and are scanned (nor has a unit over the caps of the rollup, which a REINDEX does not change); a REINDEX writes it where it fits'),
				('^key_columns=.', 10, 'key_columns', false,
				 'the units written before the option have no keys of the column (a summary, or a key list for a count(DISTINCT)), so an equality or a distinct count reads them whole (nor has a unit whose keys would take more than 3% of its bytes, which a REINDEX does not change); a REINDEX writes them where they fit'),
				('^superunit=0$', 12, 'superunit', true,
				 'the units written before the option have no summary of a group of units, so a scan of the whole index reads and folds every unit; a REINDEX writes it'),
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
	kind := 'reset';
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name,
					(SELECT string_agg(split_part(o, '=', 1), ', ' ORDER BY o) FROM unnest(c.reloptions) o
					 WHERE o ~ '^(key_cut_rows|point_columns|order_columns)=') AS removed
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_index i ON i.indexrelid = c.oid
			 WHERE am.amname = 'sift_col' AND c.relkind = 'i' AND has_table_privilege(i.indrelid, 'MAINTAIN') ORDER BY n.nspname, c.relname
	LOOP
		CONTINUE WHEN r.removed IS NULL;
		object := r.relid; command := format('ALTER INDEX %s RESET (%s);', r.name, r.removed);
		reason := format('deprecated and ignored (%s): a build does what an index without them does, and point_tid_columns lists the heap addresses that point_columns listed by value', r.removed);
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
		EXCEPTION
			WHEN lock_not_available THEN
				kind := 'skipped'; required := NULL; object := r.relid; reason := 'locked, run again'; command := NULL;
				RETURN NEXT;
				kind := 'rewrite'; required := false;
				CONTINUE;
			WHEN OTHERS THEN
				IF SQLSTATE IN ('42883', '58P01') THEN RAISE; END IF;
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
	-- the owner of the database (who may run the command) is told when the library is not preloaded; the settings it reads are for pg_read_all_settings
	kind := 'preload'; required := false; object := NULL;
	IF pg_has_role(current_user, 'pg_read_all_settings', 'USAGE')
	   AND pg_has_role(current_user, (SELECT datdba FROM pg_database WHERE datname = current_database()), 'USAGE') THEN
		command := sift._preload_hint();
		IF command IS NOT NULL THEN
			reason := 'pg_sift is not preloaded in this database: the first statement of each session is planned twice';
			RETURN NEXT;
		END IF;
	END IF;
END
$fn$;

COMMENT ON FUNCTION sift.upgrade_commands() IS 'What an update leaves to do for the indexes and tables the caller owns or may maintain, one row per command, the cheapest first: kind (reindex, reset, rewrite; skipped: locked by another session, run again; preload: the library is not preloaded), the index or table, the reason and the statement; required when queries miss rows until it runs. Changes nothing.';
