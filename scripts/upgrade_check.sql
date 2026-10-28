-- sift.upgrade_check(target): what to look at before ALTER EXTENSION pg_sift UPDATE, a library swap or a downgrade. It reads the catalogs and the
-- metapages and changes nothing. One row per finding: object, kind (version, index, table, option, setting, test), detected (the version or format minor found),
-- status (ok, warn, blocker) and advice. target, a release such as '3.1', asks what that release could not read: an index or table in a format
-- it does not know is a blocker. docs/UPGRADING.md has the commands.
--
-- scripts/upgrade-check.sh loads this file in a transaction that it rolls back, so that a database whose extension is older can be checked
-- before the update. The format minors need sift.index_format of the loaded library (this release or later; without it those rows say so).

DO $do$
BEGIN
	EXECUTE format('CREATE OR REPLACE FUNCTION sift.index_format(rel regclass, OUT major int4, OUT minor int4) RETURNS record AS %L, %L LANGUAGE C STRICT VOLATILE',
				   (SELECT probin FROM pg_proc WHERE oid = 'sift.version()'::regprocedure), 'sift_index_format');
EXCEPTION WHEN undefined_function THEN NULL;
END
$do$;

CREATE OR REPLACE FUNCTION sift.upgrade_check(target text DEFAULT NULL)
RETURNS TABLE (object text, kind text, detected text, status text, advice text)
LANGUAGE plpgsql AS $fn$
DECLARE
	-- the first release that reads each column format minor (docs/FORMATS.md section 4), and the sift.col_format_minor that writes what a release reads
	col_first CONSTANT jsonb := '{"3": "3.0", "4": "3.1", "5": "3.2", "6": "3.2", "7": "3.3", "8": "3.3", "9": "3.3", "10": "3.3", "11": "3.3", "12": "3.3", "13": "3.3", "14": "3.3", "15": "3.3", "16": "3.3", "17": "3.4", "18": "3.4", "19": "3.4", "20": "3.4", "21": "3.4", "22": "3.5", "23": "3.5", "24": "3.5", "25": "3.6", "26": "3.6", "27": "3.6", "28": "3.6", "29": "3.6", "30": "3.6", "31": "3.6", "32": "3.6"}';
	col_setting CONSTANT jsonb := '{"3.0": 3, "3.1": 4, "3.2": 6, "3.3": 15}';
	tgt text := regexp_replace(target, '^([0-9]+\.[0-9]+).*$', '\1');
	ext text;
	lib text;
	avail text;
	r record;
	maj int;
	mnr int;
	lo int;
	hi int;
	need text;
	adv text;
BEGIN
	IF target IS NOT NULL AND target !~ '^[0-9]+\.[0-9]+' THEN
		RAISE EXCEPTION 'target must be a release such as 3.1, not "%"', target;
	END IF;
	-- the library is loaded by its first call, which defines its settings
	lib := sift.version();
	SELECT extversion INTO ext FROM pg_extension WHERE extname = 'pg_sift';
	SELECT default_version INTO avail FROM pg_available_extensions WHERE name = 'pg_sift';

	object := 'pg_sift'; kind := 'version'; detected := format('extension %s, library %s', ext, lib);
	IF string_to_array(lib, '.')::int[] < string_to_array(ext, '.')::int[] THEN
		status := 'blocker'; advice := format('the library is older than the extension scripts: install pg_sift %s or later', ext);
	ELSIF string_to_array(lib, '.')::int[] > string_to_array(ext, '.')::int[] THEN
		status := 'warn'; advice := 'ALTER EXTENSION pg_sift UPDATE';
	ELSIF avail IS DISTINCT FROM ext THEN
		status := 'warn'; detected := detected || format(' (%s is available)', avail); advice := 'ALTER EXTENSION pg_sift UPDATE';
	ELSE
		status := 'ok'; advice := NULL;
	END IF;
	RETURN NEXT;

	FOR r IN SELECT c.oid::regclass::text AS rel, c.oid AS relid, am.amname, c.reloptions AS opts
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam
			 WHERE am.amname IN ('sift', 'sift_col', 'sift_table') AND c.relkind IN ('i', 'r', 'm')
			 ORDER BY 1
	LOOP
		object := r.rel; kind := CASE r.amname WHEN 'sift_table' THEN 'table' ELSE 'index' END;
		IF pg_relation_size(r.relid) = 0 THEN
			detected := 'nothing written yet'; status := 'ok'; advice := NULL;
			RETURN NEXT;
			CONTINUE;
		END IF;
		BEGIN
			EXECUTE 'SELECT major, minor FROM sift.index_format($1)' INTO maj, mnr USING r.relid;
			lo := NULL; hi := NULL;
			IF r.amname = 'sift_col' THEN
				SELECT min((to_jsonb(s)->>'format')::int), max((to_jsonb(s)->>'format')::int) INTO lo, hi FROM sift.col_segments(r.relid) s;
			ELSIF r.amname = 'sift_table' THEN
				SELECT min((to_jsonb(s)->>'format')::int), max((to_jsonb(s)->>'format')::int) INTO lo, hi FROM sift.table_segments(r.relid) s;
			END IF;
		EXCEPTION
			WHEN undefined_function THEN
				detected := 'format unknown'; status := 'warn';
				advice := 'the loaded library has no sift.index_format: install the new library files, start a new session and run this again';
				RETURN NEXT;
				CONTINUE;
			WHEN OTHERS THEN
				GET STACKED DIAGNOSTICS adv = PG_EXCEPTION_HINT;
				detected := 'unreadable'; status := 'blocker'; advice := SQLERRM || coalesce('. ' || adv, '');
				RETURN NEXT;
				CONTINUE;
		END;
		-- the first release that reads it
		need := CASE WHEN r.amname = 'sift' THEN CASE WHEN mnr >= 3 THEN '3.1' ELSE '3.0' END
					 ELSE coalesce(col_first->>greatest(hi, 3)::text, '3.0') END;
		detected := format('format %s.%s', maj, mnr) ||
				 CASE WHEN hi IS NULL THEN '' WHEN lo = hi THEN ', column format ' || hi ELSE format(', column format %s to %s', lo, hi) END;
		status := 'ok'; advice := NULL;
		IF tgt IS NOT NULL AND string_to_array(need, '.')::int[] > string_to_array(tgt, '.')::int[] THEN
			status := 'blocker';
			IF r.amname = 'sift' THEN
				adv := format('%s cannot read this format: drop the index and CREATE INDEX it again with that release, or keep this library', tgt);
			ELSIF hi = 5 THEN
				adv := format('a schema with wide leaves is written for 3.2 and later at every setting: drop the index before going back to %s', tgt);
			ELSIF hi = 17 AND tgt = '3.3' THEN
				adv := format('ALTER INDEX %s SET (key_cut = ''off''), then REINDEX INDEX %s writes units by superblock, which 3.3 reads', r.rel, r.rel);
			ELSIF hi = 20 AND tgt = '3.3' THEN
				adv := format('ALTER INDEX %s RESET (hll_columns), then REINDEX INDEX %s writes units without sketches, which 3.3 reads', r.rel, r.rel);
			ELSIF hi IN (30, 31, 32) THEN
				adv := format('ALTER INDEX %s SET (order_array_columns = ''off''), then REINDEX INDEX %s writes the sorted runs without the global order, which %s reads', r.rel, r.rel, tgt);
			ELSIF hi = 26 THEN
				adv := format('the range, inet and cidr columns of the index keep the lanes of their containment operators: drop the index before going back to %s', tgt);
			ELSIF hi = 25 THEN
				adv := format('SET sift.col_format_minor = 14, then %s writes segments without the hash of the dictionaries (and without the sections of minors 15 to 24), which %s reads',
							  CASE r.amname WHEN 'sift_table' THEN 'VACUUM FULL ' || r.rel || ' (ACCESS EXCLUSIVE lock)' ELSE 'REINDEX INDEX ' || r.rel END, tgt);
			ELSIF hi = 24 THEN
				adv := format('ALTER INDEX %s SET (count_columns = ''off'', key_columns = ''off'', key_table = off), then REINDEX INDEX %s writes segments without row counts, lists of runs and key tables, which %s reads', r.rel, r.rel, tgt);
			ELSIF hi = 23 THEN
				adv := format('ALTER INDEX %s SET (key_columns = ''off'', key_table = off), then REINDEX INDEX %s writes segments without lists of runs and key tables, which %s reads', r.rel, r.rel, tgt);
			ELSIF hi = 22 THEN
				adv := format('ALTER INDEX %s SET (key_table = off), then REINDEX INDEX %s writes segments without key tables, which %s reads', r.rel, r.rel, tgt);
			ELSIF r.amname = 'sift_col' THEN
				adv := format('SET sift.col_format_minor = %s and REINDEX INDEX %s writes what %s reads', col_setting->>tgt, r.rel, tgt);
			ELSE
				-- not ALTER TABLE .. SET ACCESS METHOD heap and back: it fails with "row is too big" on a row that does not fit a heap page, VACUUM FULL rewrites any row
				adv := format('SET sift.col_format_minor = %s, then VACUUM FULL %s rewrites it in what %s reads (ACCESS EXCLUSIVE lock)',
							  col_setting->>tgt, r.rel, tgt);
			END IF;
			advice := adv;
		END IF;
		RETURN NEXT;

		IF EXISTS (SELECT 1 FROM unnest(r.opts) o WHERE o LIKE 'mutable_limit=%') THEN
			kind := 'option'; detected := 'mutable_limit'; status := 'warn';
			advice := format('the deprecated name of write_buffer_limit: ALTER INDEX %s RESET (mutable_limit), then SET (write_buffer_limit = N)', r.rel);
			RETURN NEXT;
		END IF;
	END LOOP;

	-- the two event triggers of 3.3 call table_ddl_check, a function that only the 3.3 library has (sift_table_paths_drop is 3.2's): with an older library every ALTER TABLE
	-- and CREATE TRIGGER of the database fails
	IF tgt IS NOT NULL AND string_to_array(tgt, '.')::int[] < ARRAY[3, 3] THEN
		FOR r IN SELECT e.evtname::text AS name FROM pg_event_trigger e JOIN pg_proc p ON p.oid = e.evtfoid
				 WHERE e.evtenabled <> 'D' AND p.pronamespace = 'sift'::regnamespace AND p.proname = 'table_ddl_check' ORDER BY 1
		LOOP
			object := r.name; kind := 'trigger'; detected := 'enabled, its function is in the 3.3 library only'; status := 'blocker';
			advice := format('ALTER EVENT TRIGGER %s DISABLE before the %s library runs (it would fail every ALTER TABLE and CREATE TRIGGER), ENABLE it when 3.3 runs again', r.name, tgt);
			RETURN NEXT;
		END LOOP;
	END IF;

	-- what the units written before an option, or the segments of a sift_table below the column format of its schema, lack (the extension of 3.3 on has the
	-- function; an older one gets these rows after its update)
	IF to_regprocedure('sift.upgrade_commands()') IS NOT NULL THEN
		BEGIN
			FOR r IN SELECT * FROM sift.upgrade_commands() WHERE NOT required LOOP
				object := r.object::text; kind := CASE r.kind WHEN 'rewrite' THEN 'table' ELSE 'option' END; status := 'warn';
				detected := split_part(r.reason, ': ', 1); advice := substr(r.reason, length(detected) + 3) || '; ' || r.command;
				RETURN NEXT;
			END LOOP;
		EXCEPTION WHEN undefined_function OR undefined_file OR lock_not_available THEN NULL;
		END;
	END IF;

	-- settings outside the code: the server (file, command line), the session, ALTER DATABASE and ALTER ROLE
	FOR r IN WITH set_by AS (
				SELECT s.name, s.setting AS value, 'server (' || s.source || ')' AS scope FROM pg_settings s
				WHERE s.name LIKE 'sift.%' AND s.source NOT IN ('default', 'override', 'database', 'user', 'database user')
				UNION ALL
				SELECT split_part(c, '=', 1), substr(c, strpos(c, '=') + 1),
					   concat_ws(' ', CASE WHEN d.setrole <> 0 THEN 'ALTER ROLE ' || (SELECT rolname FROM pg_roles WHERE oid = d.setrole) END,
										CASE WHEN d.setdatabase <> 0 THEN 'IN DATABASE ' || (SELECT datname FROM pg_database WHERE oid = d.setdatabase) END)
				FROM pg_db_role_setting d, unnest(d.setconfig) c WHERE c LIKE 'sift.%'
				UNION ALL
				SELECT f.name, f.setting, 'file ' || f.sourcefile || ':' || f.sourceline FROM pg_file_settings f
				WHERE f.name LIKE 'sift.%' AND NOT EXISTS (SELECT 1 FROM pg_settings p WHERE p.name = f.name)
			 )
			 SELECT b.name AS name, b.value AS value, b.scope AS scope, p.name IS NULL AS unknown, p.vartype AS vartype, p.boot_val AS boot_val
			 FROM set_by b LEFT JOIN pg_settings p ON p.name = b.name
			 ORDER BY b.name, b.scope
	LOOP
		object := r.name; kind := 'setting'; detected := format('%s = %s, set by %s', r.name, r.value, r.scope); status := 'warn'; advice := NULL;
		IF r.name LIKE 'sift.test\_%' THEN
			kind := 'test'; advice := 'a setting of the test build only: remove it';
		ELSIF r.unknown THEN
			advice := 'the loaded library does not define it (removed, renamed or from another build): remove it, or PostgreSQL ignores it with a warning';
		ELSIF r.vartype = 'bool' AND r.boot_val = 'on' AND lower(r.value) IN ('off', 'false', 'no', '0', 'f', 'n') THEN
			advice := 'a feature is switched off here (a planner or maintenance setting that defaults on): RESET it unless that is meant';
		ELSIF r.name = 'sift.col_format_minor' AND r.value <> r.boot_val THEN
			advice := 'new column segments are written in an older format (for a standby on that release): RESET it once every node is upgraded';
		ELSE
			CONTINUE;
		END IF;
		RETURN NEXT;
	END LOOP;

	-- test-only objects left behind
	FOR r IN SELECT extname FROM pg_extension WHERE extname = 'pg_sift_test' LOOP
		object := r.extname; kind := 'test'; detected := 'extension installed'; status := 'warn';
		advice := 'it needs the test build of the library and is not meant for a normal install: DROP EXTENSION pg_sift_test';
		RETURN NEXT;
	END LOOP;
END
$fn$;
