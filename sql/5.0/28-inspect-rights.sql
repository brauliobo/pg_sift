-- The last inspectors that asked for no right, or for another rule than sift._may_inspect (sql/5.0/23-inspect.sql), and the writer that did its work before the guard asked who called.

-- cluster_advice lists the sift_col indexes of the tables the caller may look at (it read only the tables the caller could SELECT from, and never a pg_monitor member's)
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
			AND sift._may_inspect(i.indrelid)
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
		has_kc := EXISTS (SELECT 1 FROM unnest(ix.reloptions) o WHERE o ~ '^key_cut=(?!auto$)');
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

-- The checks of a followed table, made for the role that called the outermost function: the owner of the source and of the pairs, and a view the caller may read. The guard trigger of
-- sift.scope_follows asks it for each row it writes, and sift.scope_expire_pairs before it refreshes anything: a SECURITY DEFINER function does its work first and the trigger fires on
-- the UPDATE of swept_at, after the refresh.
CREATE FUNCTION sift._require_follow(f sift.scope_follows) RETURNS void LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
BEGIN
	PERFORM sift._require_owner(f.source);
	PERFORM sift._require_owner(f.pairs);
	PERFORM sift._require_select(f.definition);
END $$;
COMMENT ON FUNCTION sift._require_follow(sift.scope_follows) IS 'Internal: errors unless the role that called the outermost function owns the source and the pairs of a followed table and may read its view';

CREATE OR REPLACE FUNCTION sift._config_guard() RETURNS trigger LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	r jsonb;
BEGIN
	-- the event trigger that follows a rename writes the rows of the table that was renamed, which its owner was let rename
	IF sift._names_following() THEN
		RETURN CASE TG_OP WHEN 'DELETE' THEN OLD ELSE NEW END;
	END IF;
	FOREACH r IN ARRAY CASE TG_OP WHEN 'INSERT' THEN ARRAY[to_jsonb(NEW)] WHEN 'DELETE' THEN ARRAY[to_jsonb(OLD)] ELSE ARRAY[to_jsonb(OLD), to_jsonb(NEW)] END LOOP
		-- (the name of a table that is gone prints as its number, which to_regclass reads as the oid)
		IF TG_OP = 'DELETE' AND (NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = to_regclass(r ->> 'source')) OR (TG_TABLE_NAME = 'scope_follows' AND
			(NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = to_regclass(r ->> 'pairs')) OR NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = to_regclass(r ->> 'definition'))))) THEN
			CONTINUE;
		END IF;
		IF TG_TABLE_NAME = 'scope_follows' THEN
			PERFORM sift._require_follow(jsonb_populate_record(NULL::sift.scope_follows, r));
		ELSE
			PERFORM sift._require_owner(to_regclass(r ->> 'source'));
		END IF;
	END LOOP;
	RETURN CASE TG_OP WHEN 'DELETE' THEN OLD ELSE NEW END;
END $$;

CREATE OR REPLACE FUNCTION sift.scope_expire_pairs(pairs regclass, overlap interval DEFAULT '1 hour') RETURNS TABLE (inserted bigint, deleted bigint) LANGUAGE plpgsql AS $$
DECLARE
	f      sift.scope_follows;
	ids    bigint[];
	keys   bigint[];
	r      record;
	typ    text;
BEGIN
	inserted := 0; deleted := 0;
	FOR f IN SELECT * FROM sift.scope_follows s WHERE s.pairs = scope_expire_pairs.pairs AND s.expires IS NOT NULL ORDER BY s.source FOR UPDATE LOOP
		PERFORM sift._require_follow(f);
		SELECT format_type(a.atttypid, NULL) INTO typ FROM pg_attribute a WHERE a.attrelid = f.source AND a.attname = f.expires;
		EXECUTE sift.scope_names_sql(f, format('SELECT * FROM %s WHERE %I BETWEEN %L::%s AND %L::%s', f.source, f.expires, f.swept_at - overlap, typ, now(), typ)) INTO ids, keys;
		SELECT * INTO r FROM sift.scope_refresh_named(f, ids, keys);
		inserted := inserted + r.inserted; deleted := deleted + r.deleted;
		UPDATE sift.scope_follows s SET swept_at = now() WHERE s.pairs = f.pairs AND s.source = f.source AND s.source_column = f.source_column;
	END LOOP;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_expire_pairs: no source of "%" has an expires column', scope_expire_pairs.pairs USING ERRCODE = 'undefined_object';
	END IF;
	RETURN NEXT;
END $$;
ALTER FUNCTION sift.scope_expire_pairs(regclass, interval) SECURITY DEFINER SET search_path = pg_catalog, pg_temp;

-- The state of a parent journal is read only for a parent whose table the caller may look at: the view lists the connections of a child the caller may look at
CREATE OR REPLACE VIEW sift.col_connections AS
	SELECT c.oid::regclass AS child, l.ordinal, l.fk, l.parent_index AS parent, l.declaration, l.automatic, s.lineage, s.records, s.stamp,
		CASE WHEN f.filled = f.segments THEN 'filled'
			WHEN f.filled = 0 THEN 'unfilled'
			ELSE 'partly filled (' || f.filled || ' of ' || f.segments || ' segments)' END AS fill
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.col_connection_list(c.oid) l
	LEFT JOIN LATERAL sift.col_connection_state(CASE WHEN sift._may_inspect((SELECT p.indrelid FROM pg_index p WHERE p.indexrelid = l.parent_index))
													 THEN l.parent_index END) s ON true
	LEFT JOIN LATERAL sift.col_connection_fill(c.oid) f ON f.ordinal = l.ordinal
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid);

-- relation, kind and "table" as in sift.indexes; index keeps its name and, for a sift_table, holds the table
CREATE OR REPLACE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due,
		   c.oid::regclass AS relation, i.indrelid::regclass AS "table"
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid) AND w.segments IS NOT NULL
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error, NULL::text, c.oid::regclass, c.oid::regclass
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind IN ('r', 'm') AND sift._may_inspect(c.oid) AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index and per sift_table the caller can look at: segments, sealed write buffers, whether maintenance runs, the failures in a row, the next retry, the last error and why a sift_col index would be rewritten (due). relation is the sift object (index holds the same), kind its access method, "table" its table.';
