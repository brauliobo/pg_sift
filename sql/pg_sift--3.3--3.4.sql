\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.4'" to load this file. \quit

-- sift.upgrade_commands() also lists an index with key_cut or hll_columns whose units were written before the option (column format minors 17 and 20): the same function as in
-- pg_sift--3.4.sql, replaced so that an install updated from 3.3 gets what a fresh one does.
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
				('^key_cut=.', 17, 'key_cut', false,
				 'the units written before the option are cut by superblock, so a lookup or a range of the key reads units from every part of the heap; a REINDEX cuts them by the key'),
				('^hll_columns=.', 20, 'hll_columns', false,
				 'the units written before the option have no sketch of the column, so sift.approx_count_distinct reads their rows; a REINDEX writes it')
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

-- sift.cluster_advice() also names key_cut and key_columns (same function as in pg_sift--3.4.sql).
CREATE OR REPLACE FUNCTION sift.cluster_advice(rel regclass DEFAULT NULL, min_rows int8 DEFAULT 100000, key text DEFAULT NULL,
									OUT tbl regclass, OUT index regclass, OUT order_by text, OUT units int8, OUT units_now float8,
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
	kl text[];
	missing text;
	vm_note text;
	fq text;
	lj jsonb;
	cand text[];
	keyc text[];
	kb record;
	klost float8;
	klost_paths text;
	kcut text;
	has_kc bool;
	note text;
BEGIN
	IF key IS NOT NULL THEN
		kl := ARRAY(SELECT btrim(k) FROM unnest(string_to_array(key, ',')) k);
		IF array_length(kl, 1) IS NULL OR '' = ANY(kl) THEN
			RAISE EXCEPTION 'key must be a list of leaf names, such as ''a, b''' USING ERRCODE = 'invalid_parameter_value';
		END IF;
	END IF;
	FOR ix IN
		SELECT i.indexrelid::regclass AS idx, i.indrelid AS heap, c.reloptions, h.reltuples, h.relam, n.nspname, h.relname, h.relpages, h.relallvisible
		FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
			JOIN pg_class h ON h.oid = i.indrelid JOIN pg_namespace n ON n.oid = h.relnamespace
		WHERE a.amname = 'sift_col' AND c.relkind = 'i' AND i.indisvalid AND (rel IS NULL OR rel IN (i.indrelid, i.indexrelid))
			AND has_table_privilege(i.indrelid, 'SELECT')
		ORDER BY i.indexrelid
	LOOP
		tbl := ix.heap; index := ix.idx; order_by := NULL; units := NULL; units_now := NULL; units_after := NULL; saved_rows := NULL;
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
		fq := format('%I.%I', ix.nspname, ix.relname);
		vm_note := CASE WHEN ix.relpages > 0 AND ix.relallvisible < ix.relpages / 2.0
			THEN format('; only %s%% of the heap pages are all-visible: VACUUM (ANALYZE) %s first, or a scan checks the heap for every row of the others',
						round(100.0 * ix.relallvisible / ix.relpages), fq) ELSE '' END;
		IF kl IS NOT NULL THEN
			SELECT string_agg(k, ', ') INTO missing FROM unnest(kl) k
			WHERE k NOT IN (SELECT l.path FROM sift.col_clustering(ix.idx, 1) l WHERE l.point_units IS NOT NULL);
			IF missing IS NOT NULL THEN
				reason := format('the index has no integer, date or timestamp leaf named %s', missing);
				RETURN NEXT; CONTINUE;
			END IF;
		END IF;

		-- the leaves with what a sort by each would do: units read now and after, rows a value. An equality leaf (several rows a value, from the statistics
		-- of the column) is read at the units of its value; any other by a window of 1% of its span.
		SELECT jsonb_agg(to_jsonb(x)) INTO lj FROM (
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
				AND NOT EXISTS (SELECT 1 FROM pg_stats st WHERE st.schemaname = ix.nspname AND st.tablename = ix.relname AND st.attname = l.path)) x;
		-- the best leaf: most units saved when the table is sorted by it. A leaf that is ordered now (clustered, partial) is what a
		-- sort by another, scattered leaf would give up.
		WITH leafs AS (SELECT * FROM jsonb_to_recordset(lj) AS l(path text, lv text, total int8, rpv float8, now_u float8, after_u float8)),
		best AS (SELECT * FROM leafs WHERE kl IS NULL OR path = kl[1] ORDER BY now_u - after_u DESC, path LIMIT 1)
		SELECT best.path, best.lv, best.now_u, best.after_u, best.rpv,
			   coalesce(sum((o.total - o.now_u) * rpu), 0) AS lost, string_agg(o.path, ', ' ORDER BY o.path) AS lost_paths
		INTO b
		FROM best LEFT JOIN leafs o ON o.path <> best.path AND (kl IS NULL OR o.path <> ALL (kl)) AND o.lv IN ('clustered', 'partial') AND best.lv = 'scattered'
		GROUP BY best.path, best.lv, best.now_u, best.after_u, best.rpv;
		IF b.path IS NULL THEN
			reason := 'the index has no integer, date or timestamp leaf with zones';
			RETURN NEXT; CONTINUE;
		END IF;

		order_by := b.path;
		-- the rows of one value fill several units: a date or timestamp leaf after it sorts the rows inside the value, so a window prunes there too
		IF kl IS NOT NULL THEN
			order_by := array_to_string(kl, ', ');
		ELSIF b.rpv > rpu THEN
			SELECT a.attname INTO second FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid
			WHERE a.attrelid = ix.heap AND a.attnum > 0 AND NOT a.attisdropped AND a.attname <> b.path
				AND t.typname IN ('date', 'timestamp', 'timestamptz')
				AND a.attname IN (SELECT l.path FROM sift.col_clustering(ix.idx, 1) l WHERE l.point_units IS NOT NULL)
			ORDER BY a.attnum LIMIT 1;
			IF second IS NOT NULL THEN order_by := order_by || ', ' || second; END IF;
		END IF;
		units_now := b.now_u; units_after := b.after_u; saved_rows := ((b.now_u - b.after_u) * rpu)::int8;
		SELECT CASE WHEN ix.reltuples > 0 THEN 100.0 * s.n_mod_since_analyze / ix.reltuples END,
			   extract(epoch FROM now() - greatest(s.last_analyze, s.last_autoanalyze)) / 86400.0
		INTO churn_pct, age FROM pg_stat_all_tables s WHERE s.relid = ix.heap;
		rate := churn_pct / greatest(age, 1.0 / 24);

		SELECT EXISTS (SELECT 1 FROM pg_index u WHERE u.indrelid = ix.heap AND u.indisunique AND u.indisvalid AND u.indpred IS NULL) INTO has_unique;
		IF has_unique AND ix.relam = (SELECT oid FROM pg_am WHERE amname = 'heap') THEN
			command := format('pg_repack --table=%s --order-by=%L && psql -c %L', fq, order_by, 'VACUUM (ANALYZE) ' || fq);
		END IF;

		IF b.now_u <= b.after_u * 1.5 + 0.5 THEN
			reason := format('already ordered: %s reads %s units, a sorted table about %s', b.path, round(b.now_u::numeric, 1), round(b.after_u::numeric, 1));
		ELSIF units < 16 OR saved_rows < min_rows THEN
			reason := format('small: sorting by %s saves %s rows a query (under %s) in %s units', order_by, saved_rows, min_rows, units);
		ELSIF b.lost > 0 AND b.lost * 2 >= saved_rows THEN
			reason := format('competing leaves: sorting by %s would scatter %s, ordered now, which saves about %s rows a query against %s', order_by, b.lost_paths, round(b.lost), saved_rows);
		ELSIF rate >= 10 THEN
			reason := format('rows change too fast: %s%% modified since the last ANALYZE, %s%% a day; the order is lost faster than a repack restores it', round(churn_pct::numeric), round(rate::numeric));
		ELSE
			verdict := 'recommended';
			reason := format('an equality or range on %s reads %s units now and about %s sorted: %s rows fewer a query', b.path, round(b.now_u::numeric, 1), round(b.after_u::numeric, 1), saved_rows);
			IF b.lost > 0 THEN reason := reason || format('; it scatters %s, which saves about %s rows', b.lost_paths, round(b.lost)); END IF;
			IF command IS NULL THEN reason := reason || '; pg_repack needs a unique index (CLUSTER takes an exclusive lock)'; END IF;
			reason := reason || '; run VACUUM (ANALYZE) after the rewrite, which leaves the visibility map empty';
		END IF;
		-- the key a workload looks rows up by: a leaf of point_columns, point_tid_columns or key_columns that is not unique (or the first of key =>). key_cut keeps
		-- its units ordered through writes and merges, with no rewrite of the heap; it is only free when no leaf is ordered now (it would widen the zones of those).
		has_kc := EXISTS (SELECT 1 FROM unnest(ix.reloptions) o WHERE o ~ '^key_cut=.');
		IF NOT has_kc AND units >= 16 THEN
			cand := ARRAY(SELECT DISTINCT btrim(k) FROM unnest(ix.reloptions) o,
				LATERAL unnest(string_to_array(regexp_replace(split_part(o, '=', 2), '^auto:', ''), ',')) k
				WHERE o ~ '^(point_columns|point_tid_columns|key_columns)=' AND btrim(k) NOT IN ('', 'off', 'auto'));
			keyc := ARRAY(SELECT btrim(k) FROM unnest(ix.reloptions) o,
				LATERAL unnest(string_to_array(regexp_replace(split_part(o, '=', 2), '^auto:', ''), ',')) k WHERE o ~ '^key_columns=');
			SELECT l.path, l.now_u, l.after_u, l.rpv INTO kb
			FROM jsonb_to_recordset(lj) AS l(path text, lv text, total int8, rpv float8, now_u float8, after_u float8)
			WHERE l.path = ANY(CASE WHEN kl IS NOT NULL THEN kl[1:1] ELSE cand END) AND (l.rpv IS NULL OR l.rpv >= 2)
			ORDER BY l.now_u - l.after_u DESC, l.path LIMIT 1;
			note := NULL;
			IF kb.path IS NOT NULL AND (kb.now_u - kb.after_u) * rpu >= min_rows AND kb.now_u > kb.after_u * 1.5 + 0.5 THEN
				SELECT coalesce(sum((o.total - o.now_u) * rpu), 0), string_agg(o.path, ', ' ORDER BY o.path) INTO klost, klost_paths
				FROM jsonb_to_recordset(lj) AS o(path text, lv text, total int8, rpv float8, now_u float8, after_u float8)
				WHERE o.path <> kb.path AND (kl IS NULL OR o.path <> ALL (kl)) AND o.lv IN ('clustered', 'partial');
				kcut := coalesce(array_to_string(kl, ', '), kb.path);
				IF klost = 0 THEN
					note := format('key_cut = %L cuts the units by %s: %s reads about %s units, not %s, and keeps that through writes and merges without a rewrite of the table, and no leaf that is ordered now is widened (ALTER INDEX %s SET (key_cut = %L), then REINDEX INDEX %s; the index grows by the unit address lists)',
								   kcut, kcut, kb.path, round(kb.after_u::numeric, 1), round(kb.now_u::numeric, 1), ix.idx, kcut, ix.idx);
					IF kb.rpv IS NOT NULL AND kb.path <> ALL (keyc) AND n_rows / kb.rpv > 256 THEN
						note := note || format('; key_columns = %L keeps the keys of each unit, which lets a lookup skip the units that a few foreign rows widen', kb.path);
					END IF;
				ELSIF verdict = 'recommended' THEN
					note := format('key_cut = %L would widen %s, which are ordered now', kcut, klost_paths);
				END IF;
			ELSIF kb.path IS NULL AND b.lost = 0 AND (b.now_u - b.after_u) * rpu >= min_rows AND b.now_u > b.after_u * 1.5 + 0.5 THEN
				note := 'to keep a key ordered through writes without a rewrite, set key_cut to the column the queries look rows up by (up to four, most significant first; a column of point_columns or key_columns that is not unique is found by this function), and check it with key => ''col''';
			END IF;
			IF note IS NOT NULL THEN reason := reason || '; ' || note; END IF;
		END IF;
		reason := reason || vm_note;
		IF verdict <> 'recommended' THEN command := NULL; END IF;
		RETURN NEXT;
	END LOOP;
END
$fn$;

COMMENT ON FUNCTION sift.suggest_paths(regclass, text, int4) IS 'The paths option that a jsonb column asks for, read from a sample of its documents (evenly spaced blocks, the same answer for the same table): typed paths for the scalar keys, a wildcard for a map of more than 4 keys of one type, a group for an array of objects, free text (values of more than 64 bytes on average) left out; advice only, NULL when no key qualifies.';
-- sift.approx_count_distinct(x): an estimate of the number of distinct values of x, from a HyperLogLog sketch of 16,384 registers (relative standard error 0.81%).
-- A different function from count(DISTINCT x), which stays exact in every plan: nothing rewrites one into the other and no setting changes it. NULLs are ignored,
-- no rows give 0; every plan gives the same number for the same rows. The reloption hll_columns of a sift_col index keeps a sketch in each unit, which a scan reads
-- instead of the rows.
CREATE FUNCTION sift.approx_count_distinct_trans(internal, anyelement) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_approx_trans' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_combine(internal, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_approx_combine' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_serial(internal) RETURNS bytea
	AS 'MODULE_PATHNAME', 'sift_approx_serial' LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_deserial(bytea, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_approx_deserial' LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION sift.approx_count_distinct_final(internal) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_approx_final' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE AGGREGATE sift.approx_count_distinct(anyelement) (
	SFUNC = sift.approx_count_distinct_trans, STYPE = internal, COMBINEFUNC = sift.approx_count_distinct_combine,
	SERIALFUNC = sift.approx_count_distinct_serial, DESERIALFUNC = sift.approx_count_distinct_deserial,
	FINALFUNC = sift.approx_count_distinct_final, PARALLEL = SAFE);
COMMENT ON FUNCTION sift.approx_count_distinct_trans(internal, anyelement) IS 'Transition function of sift.approx_count_distinct: adds the extended hash of a value to a HyperLogLog sketch.';
COMMENT ON FUNCTION sift.approx_count_distinct_combine(internal, internal) IS 'Combine function of sift.approx_count_distinct: the maximum of each register of two sketches.';
COMMENT ON FUNCTION sift.approx_count_distinct_serial(internal) IS 'Serial function of sift.approx_count_distinct: the sketch as bytea, for the workers of a parallel aggregate.';
COMMENT ON FUNCTION sift.approx_count_distinct_deserial(bytea, internal) IS 'Deserial function of sift.approx_count_distinct: the sketch of a worker.';
COMMENT ON FUNCTION sift.approx_count_distinct_final(internal) IS 'Final function of sift.approx_count_distinct: the estimate of the distinct values, 0 for none.';
COMMENT ON AGGREGATE sift.approx_count_distinct(anyelement) IS 'An estimate of the distinct values of x (relative standard error 0.81%), the same in every plan; count(DISTINCT x) stays exact.';
