-- sift.col_scope_verify(index, sample, keys): the scope index against the plain join, for a sample of scope keys, under the snapshot of the caller. For each key and each scope of the index
-- it reads the facts of the key's members twice: through the scope units (sift.enable_agg, the statement the planner recognises) and through the plain plan (sift.enable_agg off), each as
-- the count and the signature of the first column of the index for each member, and reports the members that differ. A key whose answer was not read from the stored units is 'unverified'
-- with the reason; no row: every sampled key was read from the units and is the join. The keys are drawn at random from the pairs that exist now (keys: the ones named instead).
-- The statement of sift.col_scope_verify with its key, run for its plan: NULL when the scope units were read, else the reason the scan gave (EXPLAIN needs a volatile function)
CREATE FUNCTION sift._col_scope_unread(stmt text, k bigint) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	plan jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || stmt INTO plan USING k;
	IF jsonb_path_exists(plan, '$.**."Scope Units Read"') THEN RETURN NULL; END IF;
	RETURN coalesce(jsonb_path_query_first(plan, '$.**."Scope Fallback"') #>> '{}', 'the statement was not planned as a scope read');
END $$;

CREATE FUNCTION sift.col_scope_verify(index regclass, sample int DEFAULT 10, keys bigint[] DEFAULT NULL)
RETURNS TABLE (scope text, scope_key bigint, problem text, scope_rows bigint, join_rows bigint, detail text) LANGUAGE plpgsql STABLE AS $$
DECLARE
	tbl      regclass;
	schema   text;
	opts     text;
	sig_col  text;
	clause   text;
	m        text[];
	legs     text[];
	leg      text[];
	rels     text[];
	aa       text[];
	bb       text[];
	preds    text[];
	quals    text[];
	cols     text;
	pair_sql text;
	pool_sql text;
	pred     text;
	rel      regclass;
	ix       regclass;
	ks       bigint[];
	k        bigint;
	stmt     text;
	why      text;
	a        jsonb;
	b        jsonb;
	saved    text[];
	ident    text := '[\w."$]+';
BEGIN
	-- the first use of the library in a session plans its statement before the planner hook exists: load it first
	PERFORM sift.version();
	saved := ARRAY[current_setting('sift.enable_agg'), current_setting('sift.agg_force'), current_setting('sift.agg_scope')];
	SELECT x.indrelid::regclass, n.nspname, (SELECT o FROM unnest(c.reloptions) o WHERE o LIKE 'scope=%'),
		   (SELECT quote_ident(attname) FROM pg_attribute WHERE attrelid = x.indrelid AND attnum = x.indkey[0] AND x.indkey[0] > 0)
	INTO tbl, schema, opts, sig_col
	FROM pg_index x JOIN pg_class c ON c.oid = x.indexrelid JOIN pg_am am ON am.oid = c.relam
		 JOIN pg_class t ON t.oid = x.indrelid JOIN pg_namespace n ON n.oid = t.relnamespace
	WHERE x.indexrelid = col_scope_verify.index AND am.amname = 'sift_col';
	IF tbl IS NULL THEN
		RAISE EXCEPTION 'col_scope_verify: % is not a sift_col index', col_scope_verify.index USING ERRCODE = 'wrong_object_type';
	END IF;
	IF opts IS NULL OR substr(opts, 7) = 'off' THEN
		RAISE EXCEPTION 'col_scope_verify: % has no scope', col_scope_verify.index USING ERRCODE = 'object_not_in_prerequisite_state';
	END IF;
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_force', 'on', true);
	PERFORM set_config('sift.agg_scope', 'on', true);
	FOREACH clause IN ARRAY string_to_array(substr(opts, 7), ';') LOOP
		m := regexp_match(btrim(regexp_replace(clause, '\s*cols\s*\([^)]*\)\s*$', '')), '^\s*(' || ident || ')\s*:\s*(' || ident || ')\s*->\s*(.+)$');
		IF m IS NULL THEN RAISE EXCEPTION 'col_scope_verify: cannot read the scope declaration "%"', btrim(clause); END IF;
		-- the legs: (a, b) of each relation with the predicate of its recorded btree: (member, key) of the direct path, and of a joined path (via, key) and (via, member)
		quals := ARRAY[]::text[];
		rels := ARRAY[]::text[]; aa := rels; bb := rels; preds := rels;
		FOR leg IN SELECT regexp_matches(m[3], '(' || ident || ')\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)(?:\s+using\s+(' || ident || '))?', 'g') LOOP
			rel := to_regclass(CASE WHEN leg[1] LIKE '%.%' THEN leg[1] ELSE quote_ident(schema) || '.' || leg[1] END);
			ix  := to_regclass(CASE WHEN leg[4] IS NULL THEN NULL WHEN leg[4] LIKE '%.%' THEN leg[4] ELSE quote_ident(schema) || '.' || leg[4] END);
			SELECT pg_get_expr(bx.indpred, bx.indrelid) INTO pred
			FROM pg_depend d JOIN pg_index bx ON bx.indexrelid = d.refobjid
			WHERE d.objid = col_scope_verify.index AND d.refclassid = 'pg_class'::regclass AND bx.indrelid = rel AND bx.indnatts = 2
			  AND (ix IS NULL OR bx.indexrelid = ix)
			  AND (SELECT array_agg(attname::text ORDER BY attname) FROM pg_attribute WHERE attrelid = rel AND attnum = ANY (bx.indkey::int2[]))
				  = (SELECT array_agg(c ORDER BY c) FROM unnest(ARRAY[trim(both '"' from leg[2]), trim(both '"' from leg[3])]) c)
			ORDER BY bx.indisunique DESC, bx.indexrelid LIMIT 1;
			rels := rels || rel::text; aa := aa || leg[2]; bb := bb || leg[3];
			preds := preds || coalesce(pred, 'true');
			-- the columns of the predicate carry the alias of their relation, where two relations of the statement may share a name
			cols := (SELECT string_agg(regexp_replace(quote_ident(attname), '([.^$|*+?(){}\[\]\\])', '\\\1', 'g'), '|') FROM pg_attribute WHERE attrelid = rel AND attnum > 0 AND NOT attisdropped);
			quals := quals || regexp_replace(coalesce(pred, 'true'), '(?<![\w."])(' || cols || ')(?![\w"(])', '{alias}.\1', 'g');
		END LOOP;
		IF array_length(rels, 1) = 1 THEN
			pair_sql := format('SELECT p.%s FROM %s p WHERE p.%s = $1 AND %s', aa[1], rels[1], bb[1], replace(quals[1], '{alias}', 'p'));
			pool_sql := format('SELECT %s FROM %s WHERE %s', bb[1], rels[1], preds[1]);
		ELSIF array_length(rels, 1) = 3 THEN
			pair_sql := format('SELECT p.%s FROM %s p WHERE p.%s = $1 AND %s UNION SELECT v.%s FROM %s g JOIN %s v ON v.%s = g.%s WHERE g.%s = $1 AND %s AND %s',
				aa[1], rels[1], bb[1], replace(quals[1], '{alias}', 'p'), bb[3], rels[2], rels[3], aa[3], aa[2], bb[2], replace(quals[2], '{alias}', 'g'), replace(quals[3], '{alias}', 'v'));
			pool_sql := format('SELECT %s FROM %s WHERE %s UNION SELECT %s FROM %s WHERE %s', bb[1], rels[1], preds[1], bb[2], rels[2], preds[2]);
		ELSE
			RAISE EXCEPTION 'col_scope_verify: cannot read the scope declaration "%"', btrim(clause);
		END IF;
		IF col_scope_verify.keys IS NOT NULL THEN
			ks := col_scope_verify.keys;
		ELSE
			EXECUTE format('SELECT coalesce(array_agg(k), ''{}'') FROM (SELECT t.k FROM (%s) t(k) WHERE t.k IS NOT NULL GROUP BY t.k ORDER BY random() LIMIT $1) s', pool_sql)
				INTO ks USING col_scope_verify.sample;
		END IF;
		stmt := format('SELECT coalesce(jsonb_object_agg(q.member::text, jsonb_build_array(q.n, q.sig)), ''{}'') FROM (SELECT f.%s AS member, count(*) AS n, %s AS sig FROM %s f WHERE f.%s IN (%s) GROUP BY f.%s) q',
			m[2], CASE WHEN sig_col IS NULL THEN '0' ELSE format('md5(string_agg(f.%s::text, '','' ORDER BY f.%s))', sig_col, sig_col) END, tbl, m[2], pair_sql, m[2]);
		FOREACH k IN ARRAY ks LOOP
			PERFORM set_config('sift.enable_agg', 'on', true);
			why := sift._col_scope_unread(stmt, k);
			EXECUTE stmt INTO a USING k;
			PERFORM set_config('sift.enable_agg', 'off', true);
			EXECUTE stmt INTO b USING k;
			scope := m[1]; scope_key := k;
			IF a <> b THEN
				problem := 'differs';
				scope_rows := (SELECT coalesce(sum((e.v->>0)::bigint), 0) FROM jsonb_each(a) e(k, v));
				join_rows := (SELECT coalesce(sum((e.v->>0)::bigint), 0) FROM jsonb_each(b) e(k, v));
				detail := (SELECT 'differing: ' || count(*) || ', first ' || min(d.member) FROM (SELECT coalesce(x.k, y.k) AS member FROM jsonb_each(a) x(k, v)
					FULL JOIN jsonb_each(b) y(k, v) ON x.k = y.k WHERE x.v IS DISTINCT FROM y.v) d);
				RETURN NEXT;
			END IF;
			IF why IS NOT NULL THEN
				problem := 'unverified'; scope_rows := NULL; join_rows := NULL; detail := why;
				RETURN NEXT;
			END IF;
		END LOOP;
	END LOOP;
	PERFORM set_config('sift.enable_agg', saved[1], true);
	PERFORM set_config('sift.agg_force', saved[2], true);
	PERFORM set_config('sift.agg_scope', saved[3], true);
END $$;

COMMENT ON FUNCTION sift.col_scope_verify(regclass, int, bigint[]) IS 'Checks the scope index of a sift_col index against the plain join for a sample of scope keys (or the keys named), in the snapshot of the caller: the rows differ (problem differs: members whose rows differ) or the key was not read from the stored units (unverified, with the reason); no row: every sampled key was read from the units and equals the join';
