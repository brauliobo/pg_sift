-- Errors of functions released before 5.0 that did not name the function that raised them: the name leads the text, as in the other 5.0 functions.

CREATE OR REPLACE FUNCTION sift.table_find_stale_versions(tbl regclass, sample integer DEFAULT 10)
	RETURNS TABLE (duplicate_keys bigint, sample_keys text[])
	LANGUAGE plpgsql STABLE
	SET sift.enable_agg = off SET enable_indexscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off
AS $$
DECLARE
	cols text;
BEGIN
	SELECT string_agg(format('%I', a.attname), ', ' ORDER BY k.ord) INTO cols
	FROM pg_index i
	CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY k(attnum, ord)
	JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum
	WHERE i.indrelid = tbl AND i.indisprimary;
	IF cols IS NULL THEN
		RAISE EXCEPTION 'table_find_stale_versions: table % has no primary key', tbl USING ERRCODE = 'undefined_object';
	END IF;
	RETURN QUERY EXECUTE format(
		'SELECT count(*), (array_agg(k ORDER BY k))[1:%s] FROM (SELECT ROW(%s)::text AS k FROM %s GROUP BY %s HAVING count(*) > 1) d',
		sample, cols, tbl, cols);
END
$$;

CREATE OR REPLACE FUNCTION sift.facet_sql(tbl regclass, key text, query text, facets jsonb,
							   filter text DEFAULT NULL, hits bool DEFAULT false)
	RETURNS text LANGUAGE plpgsql STABLE AS $fn$
DECLARE
	n		int;
	p		record;
	i		int;
	j		int;
	fkey	text[] := '{}';
	fown	text[] := '{}';
	fsrc	text[] := '{}';
	faggs	text[] := '{}';
	fshape	text[] := '{}';
	common	text[] := '{}';
	w		text;
	rw		text[] := '{}';
	ra		text[] := '{}';
	ctes	text[] := '{}';
	shapes	text[] := '{}';
	scan	int[] := '{}';
BEGIN
	IF jsonb_typeof(facets) IS DISTINCT FROM 'array' OR jsonb_array_length(facets) = 0 THEN
		RAISE EXCEPTION 'facet_sql: facets must be a non-empty JSON array' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	n := jsonb_array_length(facets);
	FOR i IN 1..n LOOP
		SELECT * INTO p FROM sift.facet_part(facets -> (i - 1), i);
		fkey := fkey || coalesce(p.fkey, ''); fown := fown || coalesce(p.fown, ''); fsrc := fsrc || coalesce(p.fsrc, '');
		faggs := faggs || coalesce(p.faggs, ''); fshape := fshape || p.fshape;
	END LOOP;
	IF query IS NOT NULL THEN
		IF key IS NULL THEN
			RAISE EXCEPTION 'facet_sql: key, the indexed expression of the search, is required with a query' USING ERRCODE = 'null_value_not_allowed';
		END IF;
		common := common || format('(%s) @@> %L', key, query);
	END IF;
	IF filter IS NOT NULL THEN common := common || ('(' || filter || ')'); END IF;

	-- a facet counts what every other facet has selected but not its own selection: its WHERE is the search and the filter with the
	-- selections of the others. One scan per facet; the ranges with the same WHERE are the aggregates of one scan
	FOR i IN 1..n LOOP
		w := coalesce(nullif(array_to_string(common || ARRAY(SELECT o FROM unnest(fown) WITH ORDINALITY u(o, k) WHERE k <> i AND o <> ''), ' AND '), ''), 'true');
		IF fkey[i] <> '' THEN
			ctes := ctes || format('s%s AS MATERIALIZED (SELECT %s k%s, count(*) c FROM %s%s WHERE %s GROUP BY %s)', i, fkey[i], i, tbl,
				CASE WHEN fsrc[i] <> '' THEN format(' CROSS JOIN LATERAL (SELECT DISTINCT e AS sift_facet_k FROM unnest(%s) e) sift_facet_u', fsrc[i]) ELSE '' END,
				w, fkey[i]);
			scan := scan || i;
		ELSE
			j := NULL;
			SELECT x INTO j FROM generate_series(1, coalesce(array_length(rw, 1), 0)) x WHERE rw[x] = w;
			IF j IS NULL THEN rw := rw || w; ra := ra || ''::text; j := array_length(rw, 1); END IF;
			ra[j] := concat_ws(', ', nullif(ra[j], ''), faggs[i]);
			scan := scan || -j;
		END IF;
	END LOOP;
	FOR j IN 1..coalesce(array_length(rw, 1), 0) LOOP
		ctes := ctes || format('r%s AS MATERIALIZED (SELECT %s FROM %s WHERE %s)', j, ra[j], tbl, rw[j]);
	END LOOP;
	FOR i IN 1..n LOOP
		shapes := shapes || replace(replace(fshape[i], '{cte}', CASE WHEN scan[i] > 0 THEN 's' || scan[i] ELSE 'r' || -scan[i] END), '{k}', 'k' || i);
	END LOOP;
	IF hits THEN
		shapes := shapes || format('SELECT 0, 0::int8, NULL::text, ''hits'', NULL::text, NULL::text, NULL::text, count(*) FROM %s WHERE %s',
								   tbl, coalesce(nullif(array_to_string(common || ARRAY(SELECT o FROM unnest(fown) o WHERE o <> ''), ' AND '), ''), 'true'));
	END IF;
	RETURN 'WITH ' || array_to_string(ctes, E',\n     ') || E'\nSELECT facet, kind, value, lo, hi, c FROM (\n  ' ||
		   array_to_string(shapes, E'\n  UNION ALL\n  ') || E'\n) x(no, seq, facet, kind, value, lo, hi, c) ORDER BY no, seq';
END $fn$;
