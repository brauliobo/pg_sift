\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.3'" to load this file. \quit

-- Rewrites the segments of a sift_col index whose pre-aggregates are not what its options ask now (preagg, rollup, cube_minmax take effect for the
-- units written afterwards only), one segment at a time, each an atomic replacement under the maintenance lock that readers and writers do not wait
-- for. max_segments bounds the segments rewritten by this call (NULL: all, 0: only count); the call is made again to go on after a cancel or a crash.
-- refreshed: segments rewritten; remaining: stale segments this call left. The rewrite writes what sift.col_format_minor allows (4 or more).
CREATE FUNCTION sift.col_refresh(index regclass, max_segments int4 DEFAULT NULL, OUT refreshed int4, OUT remaining int4)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_refresh' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION sift.col_refresh(regclass, int4) IS 'Rewrites the segments of a sift_col index whose pre-aggregates differ from its current options, at most max_segments per call; returns the segments rewritten and those left.';

-- How well the rows of a sift_col index are ordered for its zone maps (docs/USAGE.md "Is the index clustered"), from the zones of its immutable
-- segments, one row per leaf. units: the units of the index; zoned_units: those whose zone holds a bounded range of the leaf (a unit with an
-- overflow value is always read, one with no value never); useful_pct: the share of them whose range is narrower than the leaf's; point_units:
-- the units an equality on a value of the data touches, on average; range_units: the same for a range of width_pct percent of the leaf's span;
-- range_ideal: what a column sorted by itself would touch; correlation: the order of the units against the order of their values (1 ascending,
-- -1 descending, 0 scattered); verdict: clustered, partial or scattered. Integer, date and timestamp leaves only (the others have NULLs).
CREATE FUNCTION sift.col_clustering(index regclass, width_pct float8 DEFAULT 1,
									OUT leaf int4, OUT path text, OUT units int8, OUT zoned_units int8, OUT useful_pct float8,
									OUT point_units float8, OUT range_units float8, OUT range_ideal float8, OUT correlation float8,
									OUT verdict text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_clustering' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_clustering(regclass, float8) IS 'How well the rows of a sift_col index are ordered for its zone maps, one row per leaf: the units an equality or a range touches, the correlation and a verdict (clustered, partial or scattered).';

-- Which physical order of a table (CLUSTER, or pg_repack --order-by on a heap table) would let the scans of its sift_col index skip the most units,
-- one row per index of rel (a table or an index; every index you may read when NULL). min_rows: the rows a query must save for the order to be
-- worth it (the default is what a scan reads in about a millisecond). It sorts the zones of the units virtually (docs/USAGE.md
-- "Should the table be ordered"): units_now is what an equality or range on the best leaf reads now (sift.col_clustering), units_after what a table
-- sorted by it reads (the units of the rows of one value, or of a window), saved_rows the rows a query no longer reads, churn_pct the share of the
-- rows modified since the last ANALYZE. verdict: recommended or not recommended, and reason says why (the table is small, already ordered, a
-- competing leaf that is ordered now would be scattered, the rows change faster than an order lasts, or the advice is switched off by the setting
-- sift.cluster_advice or the index option cluster_advice); command: the pg_repack call, NULL when the table has no unique index for it.
CREATE FUNCTION sift.cluster_advice(rel regclass DEFAULT NULL, min_rows int8 DEFAULT 100000,
									OUT tbl regclass, OUT index regclass, OUT key text, OUT units int8, OUT units_now float8,
									OUT units_after float8, OUT saved_rows int8, OUT churn_pct float8, OUT verdict text,
									OUT reason text, OUT command text)
	RETURNS SETOF record
	LANGUAGE plpgsql VOLATILE SET search_path = pg_catalog, pg_temp AS $fn$
DECLARE
	ix record;
	b record;
	n_rows int8;
	rpu float8;
	age float8;
	rate float8;
	second text;
	has_unique bool;
BEGIN
	FOR ix IN
		SELECT i.indexrelid::regclass AS idx, i.indrelid AS heap, c.reloptions, h.reltuples, h.relam, n.nspname, h.relname
		FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
			JOIN pg_class h ON h.oid = i.indrelid JOIN pg_namespace n ON n.oid = h.relnamespace
		WHERE a.amname = 'sift_col' AND c.relkind = 'i' AND i.indisvalid AND (rel IS NULL OR rel IN (i.indrelid, i.indexrelid))
			AND has_table_privilege(i.indrelid, 'SELECT')
		ORDER BY i.indexrelid
	LOOP
		tbl := ix.heap; index := ix.idx; key := NULL; units := NULL; units_now := NULL; units_after := NULL; saved_rows := NULL;
		churn_pct := NULL; command := NULL; verdict := 'not recommended';
		IF NOT coalesce(current_setting('sift.cluster_advice', true)::bool, true) THEN
			reason := 'switched off by the setting sift.cluster_advice';
			RETURN NEXT; CONTINUE;
		END IF;
		IF NOT coalesce((SELECT split_part(o, '=', 2) FROM unnest(ix.reloptions) o WHERE o LIKE 'cluster_advice=%'), 'true')::bool THEN
			reason := 'switched off by the option cluster_advice of the index';
			RETURN NEXT; CONTINUE;
		END IF;
		SELECT i.rows, i.units INTO n_rows, units FROM sift.col_index_info(ix.idx) i;
		IF coalesce(units, 0) = 0 THEN
			reason := 'the index has no sealed unit yet';
			RETURN NEXT; CONTINUE;
		END IF;
		rpu := n_rows::float8 / units;

		-- the best leaf: most units saved when the table is sorted by it. An equality leaf (several rows a value, from the statistics of the column)
		-- is read at the units of its value; any other by a window of 1% of its span. A leaf that is ordered now (clustered, partial) is what a
		-- sort by another, scattered leaf would give up.
		WITH leafs AS (
			SELECT l.path, l.verdict AS lv, l.units AS total, s.rpv,
				   CASE WHEN s.rpv >= 2 THEN l.point_units ELSE l.range_units END AS now_u,
				   CASE WHEN s.rpv >= 2 THEN 1 + (s.rpv - 1) / rpu ELSE l.range_ideal END AS after_u
			FROM sift.col_clustering(ix.idx, 1) l
			CROSS JOIN LATERAL (
				SELECT n_rows / greatest(1, CASE WHEN st.n_distinct > 0 THEN st.n_distinct ELSE -st.n_distinct * ix.reltuples END) AS rpv
				FROM pg_stats st WHERE st.schemaname = ix.nspname AND st.tablename = ix.relname AND st.attname = l.path
				ORDER BY st.inherited LIMIT 1) s
			WHERE l.point_units IS NOT NULL
			UNION ALL
			SELECT l.path, l.verdict, l.units, NULL, l.range_units, l.range_ideal
			FROM sift.col_clustering(ix.idx, 1) l
			WHERE l.point_units IS NOT NULL
				AND NOT EXISTS (SELECT 1 FROM pg_stats st WHERE st.schemaname = ix.nspname AND st.tablename = ix.relname AND st.attname = l.path)
		), best AS (SELECT * FROM leafs ORDER BY now_u - after_u DESC, path LIMIT 1)
		SELECT best.path, best.lv, best.now_u, best.after_u, best.rpv,
			   coalesce(sum((o.total - o.now_u) * rpu), 0) AS lost, string_agg(o.path, ', ' ORDER BY o.path) AS lost_paths
		INTO b
		FROM best LEFT JOIN leafs o ON o.path <> best.path AND o.lv IN ('clustered', 'partial') AND best.lv = 'scattered'
		GROUP BY best.path, best.lv, best.now_u, best.after_u, best.rpv;
		IF b.path IS NULL THEN
			reason := 'the index has no integer, date or timestamp leaf with zones';
			RETURN NEXT; CONTINUE;
		END IF;

		key := b.path;
		-- the rows of one value fill several units: a date or timestamp leaf after it sorts the rows inside the value, so a window prunes there too
		IF b.rpv > rpu THEN
			SELECT a.attname INTO second FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid
			WHERE a.attrelid = ix.heap AND a.attnum > 0 AND NOT a.attisdropped AND a.attname <> b.path
				AND t.typname IN ('date', 'timestamp', 'timestamptz')
				AND a.attname IN (SELECT l.path FROM sift.col_clustering(ix.idx, 1) l WHERE l.point_units IS NOT NULL)
			ORDER BY a.attnum LIMIT 1;
			IF second IS NOT NULL THEN key := key || ', ' || second; END IF;
		END IF;
		units_now := b.now_u; units_after := b.after_u; saved_rows := ((b.now_u - b.after_u) * rpu)::int8;
		SELECT CASE WHEN ix.reltuples > 0 THEN 100.0 * s.n_mod_since_analyze / ix.reltuples END,
			   extract(epoch FROM now() - greatest(s.last_analyze, s.last_autoanalyze)) / 86400.0
		INTO churn_pct, age FROM pg_stat_all_tables s WHERE s.relid = ix.heap;
		rate := churn_pct / greatest(age, 1.0 / 24);

		SELECT EXISTS (SELECT 1 FROM pg_index u WHERE u.indrelid = ix.heap AND u.indisunique AND u.indisvalid AND u.indpred IS NULL) INTO has_unique;
		IF has_unique AND ix.relam = (SELECT oid FROM pg_am WHERE amname = 'heap') THEN
			command := format('pg_repack --table=%s --order-by=%L', format('%I.%I', ix.nspname, ix.relname), key);
		END IF;

		IF b.now_u <= b.after_u * 1.5 + 0.5 THEN
			reason := format('already ordered: %s reads %s units, a sorted table about %s', b.path, round(b.now_u::numeric, 1), round(b.after_u::numeric, 1));
		ELSIF units < 16 OR saved_rows < min_rows THEN
			reason := format('small: sorting by %s saves %s rows a query (under %s) in %s units', key, saved_rows, min_rows, units);
		ELSIF b.lost > 0 AND b.lost * 2 >= saved_rows THEN
			reason := format('competing leaves: sorting by %s would scatter %s, ordered now, which saves about %s rows a query against %s', b.path, b.lost_paths, round(b.lost), saved_rows);
		ELSIF rate >= 10 THEN
			reason := format('rows change too fast: %s%% modified since the last ANALYZE, %s%% a day; the order is lost faster than a repack restores it', round(churn_pct::numeric), round(rate::numeric));
		ELSE
			verdict := 'recommended';
			reason := format('an equality or range on %s reads %s units now and about %s sorted: %s rows fewer a query', b.path, round(b.now_u::numeric, 1), round(b.after_u::numeric, 1), saved_rows);
			IF b.lost > 0 THEN reason := reason || format('; it scatters %s, which saves about %s rows', b.lost_paths, round(b.lost)); END IF;
			IF command IS NULL THEN reason := reason || '; pg_repack needs a unique index (CLUSTER takes an exclusive lock)'; END IF;
		END IF;
		IF verdict <> 'recommended' THEN command := NULL; END IF;
		RETURN NEXT;
	END LOOP;
END
$fn$;
COMMENT ON FUNCTION sift.cluster_advice(regclass, int8) IS 'Which physical order of a table (CLUSTER, pg_repack --order-by) would let the scans of its sift_col index skip the most units, one row per index: the units read now and after, the rows saved, the churn, a recommended or not recommended verdict with its reason, and the pg_repack call; off by the setting sift.cluster_advice or the index option cluster_advice.';

-- What an update to 3.3 leaves to do, one row per command, the ones that cost least first (a REINDEX CONCURRENTLY before a VACUUM FULL, and what queries
-- need before what they only gain from). kind: reindex or rewrite; object: the index or table; reason: what the command changes; command: the statement;
-- required: queries miss rows until it runs (a word over 255 bytes is stored as its start and a hash since 3.3, and a text index of an older release holds
-- it cut at 255 bytes; the index does not say whether it holds one, so every text index except analyzer = keyword is listed, and stays listed after its
-- REINDEX). Otherwise it is a gain: a sift_col index with an option (cube_minmax, JSON paths, rollup_dims, set_columns) that only the units written after
-- it follow, set on an index whose older units lack the section; a sift_table (it cannot be reindexed) whose segments are below the column format its
-- schema is written in now (calendar cells for date and timestamp columns, minor 8; complete JSON kinds for declared paths, minor 6). A VACUUM FULL takes
-- an ACCESS EXCLUSIVE lock and rewrites the table in the format of sift.col_format_minor: schedule it. Reads the catalogs and the metapages, changes
-- nothing, and gives up on a relation that is locked at once (lock_timeout) or that this library refuses (sift.upgrade_check reports those); a library
-- that cannot be loaded is an error. ALTER EXTENSION pg_sift UPDATE to 3.3 prints this list.
CREATE FUNCTION sift.upgrade_commands(OUT kind text, OUT object regclass, OUT reason text, OUT command text, OUT required bool)
	RETURNS SETOF record
	LANGUAGE plpgsql SET lock_timeout = '100ms' AS $fn$
DECLARE
	r record;
	lo int;
	why text;
BEGIN
	kind := 'reindex'; required := true;
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace
			 WHERE am.amname = 'sift' AND c.relkind = 'i' AND 'analyzer=keyword' <> ALL (coalesce(c.reloptions, '{}')) ORDER BY n.nspname, c.relname
	LOOP
		object := r.relid; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		reason := 'long words (over 255 bytes) are found only after REINDEX: an index built before 3.3 holds such a word cut at 255 bytes, so a query for it finds nothing';
		RETURN NEXT;
	END LOOP;
	required := false;
	FOR r IN SELECT c.oid AS relid, c.reloptions AS opts, format('%I.%I', n.nspname, c.relname) AS name
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace
			 WHERE am.amname = 'sift_col' AND c.relkind = 'i' AND c.reloptions IS NOT NULL ORDER BY n.nspname, c.relname
	LOOP
		BEGIN
			SELECT min((to_jsonb(s)->>'format')::int) INTO lo FROM sift.col_segments(r.relid) s;
		EXCEPTION WHEN OTHERS THEN
			IF SQLSTATE IN ('42883', '58P01', '55P03') THEN RAISE; END IF;
			CONTINUE;
		END;
		CONTINUE WHEN lo IS NULL;
		-- a segment is minor 9 or 10 when some unit holds the section, so a lower minor means no unit of the segment does (docs/COL.md 2.13)
		SELECT string_agg(format('%s with column format %s: %s', v.what, lo, v.why), '; ' ORDER BY v.minor) INTO why FROM (VALUES
				('^paths=', 6, 'JSON paths',
				 'segments before format 6 may lack the kinds of a JSON null or of a type; a predicate that needs them uses the normal plan, and a REINDEX writes them'),
				('^cube_minmax=(true|on)$', 7, 'cube_minmax',
				 'the units written before the option scan; a REINDEX writes the minimum and maximum of every unit'),
				('^rollup_dims=.', 9, 'rollup_dims',
				 'the units written before the option have no rollup of their rows and are scanned (nor has a unit over the caps of the rollup, which a REINDEX does not change); a REINDEX writes it where it fits'),
				('^set_columns=.', 10, 'set_columns',
				 'the units written before the option have no small-set summary and are read (nor has a unit whose keys are all alike or fill their range); a REINDEX writes it where it helps')
			 ) v(pat, minor, what, why)
			 WHERE lo < v.minor AND EXISTS (SELECT 1 FROM unnest(r.opts) opt WHERE opt ~ v.pat);
		CONTINUE WHEN why IS NULL;
		object := r.relid; reason := why; command := format('REINDEX INDEX CONCURRENTLY %s;', r.name);
		RETURN NEXT;
	END LOOP;
	kind := 'rewrite';
	FOR r IN SELECT c.oid AS relid, format('%I.%I', n.nspname, c.relname) AS name,
					EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
							AND a.atttypid IN ('date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype)) AS calendar,
					EXISTS (SELECT 1 FROM sift.table_paths p WHERE p.relid = c.oid) AS paths
			 FROM pg_class c JOIN pg_am am ON am.oid = c.relam JOIN pg_namespace n ON n.oid = c.relnamespace
			 WHERE am.amname = 'sift_table' AND c.relkind IN ('r', 'm') ORDER BY n.nspname, c.relname
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
COMMENT ON FUNCTION sift.upgrade_commands() IS 'What an update leaves to do, one row per command, the cheapest first: kind (reindex, rewrite), the index or table, the reason and the statement; required when queries miss rows until it runs. Changes nothing.';

-- What the Custom Scan (SiftAgg) scans of this session did (docs/USAGE.md "Session statistics"): one row per counter, and one per refusal
-- slug recorded (sift.explain_refusals). The counters are in the memory of the backend: no shared memory, no shared_preload_libraries, so the
-- figures are of the session and lost with it.
CREATE FUNCTION sift.stats(OUT name text, OUT value int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_stats' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.stats() IS 'What the aggregate pushdown (Custom Scan (SiftAgg)) scans of this session did: one row per counter and per refusal slug.';

CREATE FUNCTION sift.stats_reset() RETURNS void
	AS 'MODULE_PATHNAME', 'sift_stats_reset' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.stats_reset() IS 'Resets the session counters that sift.stats() reports.';

-- Why each candidate path (arm) of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run (docs/USAGE.md "Why a
-- query kept the normal plan"): one row per arm, used = the arm is in the plan, slug and reason = the first reason it gave up for (NULL when used),
-- scan_cost and normal_cost = the costs of the path that was built and of the normal plan it was compared with (NULL when there was no such path).
-- Needs the privilege to select from the tables of the query; it changes no setting of the session.
CREATE FUNCTION sift.explain_why(query text, OUT arm text, OUT used bool, OUT slug text, OUT reason text, OUT scan_cost float8, OUT normal_cost float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_explain_why' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.explain_why(text) IS 'Why each candidate path of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run: one row per arm with the first reason it gave up.';

-- The slugs sift.explain_why and sift.stats() name, with their class: 'limit' when the query has more of something than a fixed limit of the
-- extension allows (docs/LIMITS.md), 'semantic' otherwise.
CREATE FUNCTION sift.refusal_slugs(OUT slug text, OUT class text, OUT reason text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_agg_refusal_slugs' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.refusal_slugs() IS 'The slugs that sift.explain_why and sift.stats() name, with their class (limit: the query has more of something than a fixed limit of the extension allows; semantic: otherwise) and the reason.';

-- The scans of this session that could not answer from the pre-aggregates of their index, by index and reason (a slug), the units scanned for
-- lack of a rollup, and whether other options of the index (rollup, preagg) would answer them (fixable). Most scans first.
CREATE FUNCTION sift.col_rollup_misses(OUT rel regclass, OUT reason text, OUT fixable bool, OUT scans int8, OUT units int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_rollup_misses' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_rollup_misses() IS 'The scans of this session that could not answer from the pre-aggregates of their sift_col index, by index and reason, and whether other index options would answer them.';

-- Operator classes that let the planner use a sift_col index as a bitmap index scan (docs/COL-SCAN.md; sift.enable_col_scan, off by
-- default). Opt-in per column: CREATE INDEX ON t USING sift_col (id sift.col_int4_ops, ts sift.col_timestamptz_ops). The stored index
-- is the one of sift.col_ops. Strategies are btree's: 1 <, 2 <=, 3 =, 4 >=, 5 >; text and uuid keep the equality only (their order is
-- the collation's, not the dictionary's), text adds 7 LIKE and 8 ^@ (a literal prefix); bpchar is char(n) only, numeric numeric(p <= 18, s) only (CREATE INDEX refuses the others).
DO $$
DECLARE
	t text;
	u text;
	n int;
	ops text[] := ARRAY['<', '<=', '=', '>=', '>'];
	ints text[] := ARRAY['int2', 'int4', 'int8'];
	list text;
BEGIN
	CREATE OPERATOR FAMILY sift.col_integer_ops USING sift_col;
	FOREACH t IN ARRAY ints LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col FAMILY sift.col_integer_ops AS %s', t, t, list);
	END LOOP;
	FOREACH t IN ARRAY ints LOOP
		FOREACH u IN ARRAY ints LOOP
			CONTINUE WHEN t = u;
			list := '';
			FOR n IN 1..5 LOOP
				list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, u);
			END LOOP;
			EXECUTE format('ALTER OPERATOR FAMILY sift.col_integer_ops USING sift_col ADD %s', list);
		END LOOP;
	END LOOP;
	CREATE OPERATOR FAMILY sift.col_float_ops USING sift_col;
	FOREACH t IN ARRAY ARRAY['float4', 'float8'] LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col FAMILY sift.col_float_ops AS %s', t, t, list);
	END LOOP;
	FOREACH t IN ARRAY ARRAY['float4', 'float8'] LOOP
		FOREACH u IN ARRAY ARRAY['float4', 'float8'] LOOP
			CONTINUE WHEN t = u;
			list := '';
			FOR n IN 1..5 LOOP
				list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, u);
			END LOOP;
			EXECUTE format('ALTER OPERATOR FAMILY sift.col_float_ops USING sift_col ADD %s', list);
		END LOOP;
	END LOOP;
	FOREACH t IN ARRAY ARRAY['date', 'timestamp', 'timestamptz', 'bool', 'numeric'] LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], t, t);
		END LOOP;
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col AS %s', t, t, list);
	END LOOP;
	EXECUTE 'CREATE OPERATOR CLASS sift.col_text_ops FOR TYPE text USING sift_col AS OPERATOR 3 pg_catalog.= (text, text), OPERATOR 7 pg_catalog.~~ (text, text), OPERATOR 8 pg_catalog.^@ (text, text)';
	FOREACH t IN ARRAY ARRAY['bpchar', 'uuid'] LOOP
		EXECUTE format('CREATE OPERATOR CLASS sift.col_%s_ops FOR TYPE %s USING sift_col AS OPERATOR 3 pg_catalog.= (%s, %s)', t, t, t, t);
	END LOOP;
END
$$;

-- sift.table_space reports the virtual blocks of TIDs left: the function is replaced, its columns changed.
DROP FUNCTION sift.table_space(regclass);

-- The space of a sift_table: relation_blocks (the file), high_water (blocks ever used), free_blocks (on the free list, of which
-- reusable_blocks can be taken now, the rest wait for snapshots that may still read them), free_extents, vblocks_left (the virtual
-- blocks of TIDs not handed out yet, up to 291 rows each; they are never reused, and VACUUM FULL renumbers them).
CREATE FUNCTION sift.table_space(tbl regclass,
								 OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8,
								 OUT free_extents int8, OUT vblocks_left int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_table_space' LANGUAGE C STRICT VOLATILE;

-- 3.2.0 and 3.2.1 ran the trigger that clears the paths of a dropped table as the dropping role, which has no access to sift.table_paths: every DROP
-- (a table, an index, a schema) by a role other than the extension's owner and superusers failed with "permission denied for schema sift", and
-- sift.table_set_paths failed for the owner of a table. Both run as the extension's owner now (the check of the table's owner is the session's role).
ALTER FUNCTION sift.table_paths_drop() SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
ALTER FUNCTION sift.table_set_paths(regclass, name, text) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;

-- A column added to a sift_table past the 127 it holds (or a column of declared paths renamed or retyped into the excess) fails the ALTER
-- TABLE, as CREATE TABLE does, instead of the next write.
CREATE FUNCTION sift.table_ddl_check() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_table_ddl_check' LANGUAGE C;

CREATE EVENT TRIGGER sift_table_ddl_check ON ddl_command_end WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION sift.table_ddl_check();

-- pg_repack copies a table into a heap table and swaps the files: its trigger repack.repack_trigger is refused on a sift_table, which CLUSTER or
-- VACUUM FULL rewrite.
CREATE EVENT TRIGGER sift_table_repack_guard ON ddl_command_start WHEN TAG IN ('CREATE TRIGGER') EXECUTE FUNCTION sift.table_ddl_check();


-- Maintenance state of an index: segments and sealed write buffers waiting for promotion, running (a promotion, merge, VACUUM or verify holds
-- the maintenance lock; NULL on a standby) and, when its last background step failed, the number of failures in a row, when, the earliest the
-- background tries again, the SQLSTATE and the message (the log has them too, this keeps the last). NULL while a REINDEX holds the index.
CREATE FUNCTION sift.worker_state(index regclass,
								  OUT segments int4, OUT sealed int4, OUT running bool, OUT failures int4,
								  OUT failed_at timestamptz, OUT next_retry timestamptz, OUT error_code text, OUT last_error text)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_worker_state' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.worker_state(regclass) IS 'Maintenance state of an index: segments and sealed write buffers waiting for promotion, whether a step runs now, and the last background failure with its retry time.';

-- One row per sift and sift_col index of the database whose table the caller can read; due: why a sift_col index would be rewritten now
-- (sift.col_maintenance_due). A failed step of a sift_col worker is kept like a BM25 one.
CREATE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index the caller can read: its maintenance state (sift.worker_state) and, for a sift_col index, why it would be rewritten now.';
-- it lists what the caller can read (has_table_privilege on the table), so every role may select from it; the two rollup views of 3.2 list what
-- functions open to every role return, and only superusers could read them
GRANT SELECT ON sift.workers, sift.col_rollup_state, sift.col_rollup_requests TO PUBLIC;

-- Logical replication of a sift_table needs pg_sift in shared_preload_libraries (docs/COL-TAM.md section 5): a custom resource manager can only
-- be registered at preload time. Without it the first DML on a published table fails; this trigger refuses CREATE/ALTER PUBLICATION instead, when
-- the publication would carry a sift_table (wal_level = logical, a permanent table, a publication that publishes inserts, updates or deletes).
-- It fires for every role that creates a publication and reads shared_preload_libraries, which only roles with pg_read_all_settings may examine: it
-- runs as the extension's owner (the check of who may publish what is the server's).
CREATE FUNCTION sift.publication_check() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
DECLARE
	tbl regclass;
BEGIN
	IF current_setting('wal_level') <> 'logical' OR current_setting('shared_preload_libraries') ~ '(^|[\s,"/])pg_sift([\s,".]|$)' THEN
		RETURN;
	END IF;
	SELECT g.relid::regclass INTO tbl
		FROM pg_event_trigger_ddl_commands() d
			JOIN pg_publication p ON p.oid = CASE d.classid
					WHEN 'pg_publication'::regclass THEN d.objid
					WHEN 'pg_publication_rel'::regclass THEN (SELECT r.prpubid FROM pg_publication_rel r WHERE r.oid = d.objid)
					WHEN 'pg_publication_namespace'::regclass THEN (SELECT n.pnpubid FROM pg_publication_namespace n WHERE n.oid = d.objid)
				END
			CROSS JOIN LATERAL pg_get_publication_tables(p.pubname) g
			JOIN pg_class c ON c.oid = g.relid
			JOIN pg_am a ON a.oid = c.relam
		WHERE a.amname = 'sift_table' AND c.relpersistence = 'p' AND (p.pubinsert OR p.pubupdate OR p.pubdelete)
		LIMIT 1;
	IF tbl IS NOT NULL THEN
		RAISE EXCEPTION 'sift_table table "%" is published and wal_level is "logical", but pg_sift is not in shared_preload_libraries', tbl
			USING ERRCODE = 'feature_not_supported',
				DETAIL = 'Logical replication of a sift_table table needs the pg_sift resource manager, which is registered only when the library is preloaded.',
				HINT = format('Add pg_sift to shared_preload_libraries (primary and every standby) and restart, or publish a heap staging table instead of "%s".', tbl);
	END IF;
END
$fn$;

CREATE EVENT TRIGGER sift_publication_check ON ddl_command_end WHEN TAG IN ('CREATE PUBLICATION', 'ALTER PUBLICATION')
	EXECUTE FUNCTION sift.publication_check();

-- 3.3 writes column formats up to 12 by default (sift.col_format_minor), which a 3.2 library refuses; said plainly once, as a warning because the script
-- runs at warning level and the user decides whether a standby or a rollback still needs 3.2.
DO $do$
BEGIN
	RAISE WARNING 'pg_sift 3.3: a 3.2 library cannot read the sift_table tables and sift_col indexes written after this update, unless sift.col_format_minor is lowered to 6 first'
		USING HINT = 'The default is 12 in 3.3. Set 6 (3.2), 4 (3.1) or 3 (3.0) for a standby or a rollback that stays on an older release; SELECT * FROM sift.upgrade_check(''3.2'') lists what it cannot read (docs/UPGRADING.md section 4).';
END
$do$;

-- What this update leaves to do, printed once; sift.upgrade_commands() lists it again. The last step of every update path to 3.3, so it covers what 3.0,
-- 3.1 and 3.2 wrote. It only reads, and never fails the update: a library that does not answer (an older one loaded in this session, a relation locked by
-- another session) gets a warning that says to run the function after reconnecting. An extension script runs with client_min_messages at warning or
-- above, so what asks for an action is a warning, and the all-clear lowers it to notice for itself (undone with the script). The text indexes are a
-- warning of their own: a query for a word over 255 bytes misses rows in them until the REINDEX.
DO $do$
DECLARE
	g record;
	listed bool := false;
	msg text[] := ARRAY['pg_sift 3.3: REINDEX is needed for %s text index(es): a query for a word over 255 bytes finds nothing in an index built before this release',
						'pg_sift 3.3: REINDEX is worth it for %s sift_col index(es) after this update',
						'pg_sift 3.3: a rewrite is worth it for %s sift_table table(s) after this update'];
	hint text[] := ARRAY['Such words were cut at 255 bytes, and the index does not say whether it holds one, so every text index (analyzer other than keyword) is listed. REINDEX CONCURRENTLY cannot run in a transaction block. SELECT * FROM sift.upgrade_commands() lists them again.',
						 'The older units stay readable until then. REINDEX CONCURRENTLY cannot run in a transaction block. SELECT * FROM sift.upgrade_commands() gives the reason for each index.',
						 'The older segments stay readable until then. VACUUM FULL takes an ACCESS EXCLUSIVE lock and rewrites the table: schedule it. SELECT * FROM sift.upgrade_commands() gives the reason for each table.'];
BEGIN
	FOR g IN SELECT grp, count(*) AS n, array_agg(command ORDER BY ord) AS cmds
			 FROM (SELECT CASE WHEN required THEN 1 WHEN kind = 'reindex' THEN 2 ELSE 3 END AS grp, row_number() OVER () AS ord, command
				   FROM sift.upgrade_commands()) s GROUP BY grp ORDER BY grp
	LOOP
		listed := true;
		RAISE WARNING '%', format(msg[g.grp], g.n)
			USING DETAIL = concat(array_to_string(g.cmds[1:20], E'\n'), CASE WHEN g.n > 20 THEN format(E'\nand %s more', g.n - 20) END), HINT = hint[g.grp];
	END LOOP;
	IF NOT listed THEN
		PERFORM set_config('client_min_messages', 'notice', true);
		RAISE NOTICE 'pg_sift 3.3: nothing to REINDEX or rewrite after this update';
	END IF;
EXCEPTION WHEN OTHERS THEN
	RAISE WARNING 'pg_sift 3.3: the check for what to REINDEX or rewrite did not run (%): reconnect and run SELECT * FROM sift.upgrade_commands()', SQLERRM;
END
$do$;

COMMENT ON FUNCTION sift.worker_state(regclass) IS 'Maintenance state of one sift or sift_col index: directory entries, sealed write buffers, whether maintenance runs and, when the last background step failed, the failures in a row, when, the earliest retry, the SQLSTATE and the message. Read through sift.workers.';
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index whose table you can read: segments, sealed write buffers, whether maintenance runs, the failures in a row of the background, when, the next retry, the SQLSTATE and the last error, and why a sift_col index would be rewritten (due).';
