\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.1'" to load this file. \quit

-- Facet counts over a search (docs/FIELDS-FACETS.md): terms, range, histogram and date_histogram facets of any column type, several in one
-- statement, with the facets of a multi-select (drill-sideways) and a top N with an "other" count. sift.facet_sql builds the SQL statement
-- (one `GROUP BY` scan per facet, the search as one `key @@> query` conjunct of each; the ranges of a facet list share one scan of
-- FILTER aggregates) that sift.facet_counts runs: with a sift_col index on the columns each scan runs as Custom Scan (SiftAgg) with the
-- search as the row selection, without one it is an ordinary plan and still exact.

-- Raises when a gap-filled histogram would return more than 10000 buckets; returns its argument otherwise.
CREATE FUNCTION sift.facet_cap(buckets int8) RETURNS int8
	LANGUAGE plpgsql VOLATILE PARALLEL SAFE AS $fn$
BEGIN
	IF buckets > 10000 THEN
		RAISE EXCEPTION 'a facet with "fill" would return more than 10000 buckets' USING ERRCODE = 'program_limit_exceeded',
			HINT = 'Use a larger interval or unit, or leave "fill" off.';
	END IF;
	RETURN buckets;
END $fn$;

-- One facet of sift.facet_sql: its name, the group key (NULL for a range: its buckets are the FILTER aggregates faggs), its own filter (the
-- "selected" values and "filter" of the spec, NULL if none), the array its key comes from unnesting (NULL: none) and the SQL that shapes the
-- rows of its scan into (facet_no, seq, facet, kind, value, lo, hi, count); {cte} and {k} stand for the scan and its key column.
CREATE FUNCTION sift.facet_part(spec jsonb, no int,
								OUT fname text, OUT fkey text, OUT fown text, OUT fsrc text, OUT faggs text, OUT fshape text)
	LANGUAGE plpgsql STABLE AS $fn$
DECLARE
	s		jsonb := CASE WHEN jsonb_typeof(spec) = 'string' THEN jsonb_build_object('expr', spec #>> '{}') ELSE spec END;
	ex		text;
	t		text;
	sel		text;
	lits	text;
	ord		text;
	sz		int;
	mx		text;
	ranked	text;
	rng		text;
	itv		numeric;
	off		numeric;
	unit	text;
	step	text;
	edges	text[];
	n		int;
	conds	text[];
	lo		text;
	hi		text;
	k		text := '{k}';
	c		text := '{cte}';
	miss	text := '';
	arr		text[];
BEGIN
	IF jsonb_typeof(s) IS DISTINCT FROM 'object' THEN
		RAISE EXCEPTION 'facet %: a facet is a column expression or an object', no USING ERRCODE = 'invalid_parameter_value';
	END IF;
	ex := coalesce(s->>'expr', s->>'name');
	IF ex IS NULL THEN
		RAISE EXCEPTION 'facet %: "expr" or "name" is required', no USING ERRCODE = 'invalid_parameter_value';
	END IF;
	fname := coalesce(s->>'name', ex);
	t := coalesce(s->>'type', CASE WHEN s ? 'edges' THEN 'range' WHEN s ? 'unit' THEN 'date_histogram'
								   WHEN s ? 'interval' THEN 'histogram' ELSE 'terms' END);
	IF t NOT IN ('terms', 'range', 'histogram', 'date_histogram') THEN
		RAISE EXCEPTION 'facet %: unknown type "%" (terms, range, histogram, date_histogram)', no, t USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF coalesce((s->>'array')::bool, false) AND t <> 'terms' THEN
		RAISE EXCEPTION 'facet %: "array" is for terms facets', no USING ERRCODE = 'invalid_parameter_value';
	END IF;
	lits := (SELECT string_agg(quote_literal(v), ', ') FROM jsonb_array_elements_text(coalesce(s->'selected', '[]')) v);
	IF coalesce((s->>'missing')::bool, false) AND t <> 'range' THEN
		miss := format(' UNION ALL SELECT %s, 4611686018427387905, %L, ''missing'', NULL::text, NULL::text, NULL::text, c FROM %s WHERE %s IS NULL',
					   no, fname, c, k);
	END IF;

	IF t = 'terms' THEN
		fsrc := CASE WHEN coalesce((s->>'array')::bool, false) THEN ex END;
		fkey := CASE WHEN fsrc IS NOT NULL THEN 'sift_facet_u.sift_facet_k' ELSE '(' || ex || ')' END;
		sel := CASE WHEN lits IS NULL THEN NULL
					WHEN fsrc IS NOT NULL THEN format('(%s)::text[] && ARRAY[%s]::text[]', ex, lits)
					ELSE format('(%s) IN (%s)', ex, lits) END;
		sz := (s->>'size')::int;
		ord := lower(coalesce(s->>'order', 'count'));
		ord := CASE ord WHEN 'count' THEN 'c DESC, ' || k WHEN 'count desc' THEN 'c DESC, ' || k WHEN 'count asc' THEN 'c, ' || k
						WHEN 'value' THEN k WHEN 'value asc' THEN k WHEN 'value desc' THEN k || ' DESC' END;
		IF ord IS NULL THEN
			RAISE EXCEPTION 'facet %: "order" is count, count asc, value or value desc', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		ranked := format('(SELECT %s k, c, row_number() OVER (ORDER BY %s) rn FROM %s WHERE %s IS NOT NULL) t', k, ord, c, k);
		fshape := format('SELECT %s, rn, %L, ''value'', k::text, NULL::text, NULL::text, c FROM %s%s', no, fname, ranked,
						 CASE WHEN sz IS NULL THEN '' ELSE format(' WHERE rn <= %s', sz) END);
		IF sz IS NOT NULL THEN
			fshape := fshape || format(' UNION ALL SELECT %s, 4611686018427387904, %L, ''other'', NULL::text, count(*)::text, NULL::text, sum(c)::int8 FROM %s WHERE rn > %s HAVING count(*) > 0',
									   no, fname, ranked, sz);
		END IF;
	ELSIF t = 'range' THEN
		edges := ARRAY(SELECT quote_literal(v) FROM jsonb_array_elements_text(s->'edges') v);
		n := coalesce(array_length(edges, 1), 0);
		IF n = 0 THEN
			RAISE EXCEPTION 'facet %: "edges" must list at least one bound', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		-- bucket i is [edges[i], edges[i + 1]); the first and the last are open: the buckets are FILTER aggregates of one scan without keys
		conds := ARRAY(SELECT concat_ws(' AND ', CASE WHEN i > 1 THEN format('(%s) >= %s', ex, edges[i - 1]) END,
												 CASE WHEN i <= n THEN format('(%s) < %s', ex, edges[i]) END) FROM generate_series(1, n + 1) i);
		faggs := (SELECT string_agg(format('count(*) FILTER (WHERE %s) a%s_%s', conds[i], no, i - 1), ', ' ORDER BY i) FROM generate_series(1, n + 1) i);
		IF coalesce((s->>'missing')::bool, false) THEN
			faggs := faggs || format(', count(*) FILTER (WHERE (%s) IS NULL) a%s_m', ex, no);
			miss := format(' UNION ALL SELECT %s, 4611686018427387905, %L, ''missing'', NULL::text, NULL::text, NULL::text, a%s_m FROM %s WHERE a%s_m > 0',
						   no, fname, no, c, no);
		END IF;
		arr := ARRAY(SELECT format('a%s_%s', no, i - 1) FROM generate_series(1, n + 1) i);
		rng := (SELECT string_agg(format('(%s, %L, %s, %s)', i - 1,
							'[' || CASE WHEN i = 1 THEN '' ELSE trim(both '''' from edges[i - 1]) END || ',' ||
								   CASE WHEN i = n + 1 THEN '' ELSE trim(both '''' from edges[i]) END || ')',
							CASE WHEN i = 1 THEN 'NULL::text' ELSE edges[i - 1] || '::text' END,
							CASE WHEN i = n + 1 THEN 'NULL::text' ELSE edges[i] || '::text' END), ', ' ORDER BY i)
				FROM generate_series(1, n + 1) i);
		fshape := format('SELECT %s, b.i, %L, ''bucket'', b.label, b.lo, b.hi, (ARRAY[%s])[b.i + 1] FROM (VALUES %s) b(i, label, lo, hi) CROSS JOIN %s',
						 no, fname, array_to_string(arr, ', '), rng, c);
		-- the buckets of the selection as comparisons the column scan can take
		sel := CASE WHEN lits IS NULL THEN NULL ELSE
			(SELECT '(' || string_agg('(' || conds[i] || ')', ' OR ') || ')' FROM generate_series(1, n + 1) i
			 WHERE (i - 1)::text IN (SELECT jsonb_array_elements_text(s->'selected'))) END;
	ELSIF t = 'histogram' THEN
		itv := (s->>'interval')::numeric;
		off := coalesce((s->>'offset')::numeric, 0);
		IF itv IS NULL OR itv <= 0 THEN
			RAISE EXCEPTION 'facet %: "interval" must be a positive number', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		fkey := format('floor(((%s)::numeric - %s) / %s)', ex, off, itv);
		sel := CASE WHEN lits IS NULL THEN NULL ELSE (SELECT '(' || string_agg(format('(%s) >= %s AND (%s) < %s', ex, v::numeric, ex, v::numeric + itv), ' OR ') || ')'
													 FROM jsonb_array_elements_text(s->'selected') v) END;
		lo := format('k * %s + %s', itv, off);
		hi := format('(k + 1) * %s + %s', itv, off);
		IF coalesce((s->>'fill')::bool, false) THEN
			mx := format('(SELECT s.k, coalesce(r.c, 0) c FROM (SELECT generate_series(a.mn, a.mn + sift.facet_cap((a.mx - a.mn)::int8)) k '
						 'FROM (SELECT min(%s) mn, max(%s) mx FROM %s WHERE %s IS NOT NULL) a WHERE a.mn IS NOT NULL) s '
						 'LEFT JOIN (SELECT %s k, c FROM %s) r ON r.k = s.k) d', k, k, c, k, k, c);
		ELSE
			mx := format('(SELECT %s k, c FROM %s WHERE %s IS NOT NULL) d', k, c, k);
		END IF;
		fshape := format('SELECT %s, row_number() OVER (ORDER BY k), %L, ''bucket'', (%s)::text, (%s)::text, (%s)::text, c FROM %s',
						 no, fname, lo, lo, hi, mx);
	ELSE
		unit := lower(s->>'unit');
		step := CASE unit WHEN 'second' THEN '1 second' WHEN 'minute' THEN '1 minute' WHEN 'hour' THEN '1 hour' WHEN 'day' THEN '1 day'
						  WHEN 'week' THEN '1 week' WHEN 'month' THEN '1 month' WHEN 'quarter' THEN '3 months' WHEN 'year' THEN '1 year' END;
		IF step IS NULL THEN
			RAISE EXCEPTION 'facet %: "unit" is second, minute, hour, day, week, month, quarter or year', no USING ERRCODE = 'invalid_parameter_value';
		END IF;
		fkey := format('date_trunc(%L, (%s)::timestamp)', unit, ex);
		sel := CASE WHEN lits IS NULL THEN NULL ELSE (SELECT '(' || string_agg(format('(%s)::timestamp >= %L::timestamp AND (%s)::timestamp < %L::timestamp + interval %L',
													ex, v, ex, v, step), ' OR ') || ')' FROM jsonb_array_elements_text(s->'selected') v) END;
		IF coalesce((s->>'fill')::bool, false) THEN
			mx := format('(SELECT s.k, coalesce(r.c, 0) c FROM (SELECT a.k, sift.facet_cap(count(*) OVER ()) FROM (SELECT generate_series(a.mn, a.mx, interval %L) k '
						 'FROM (SELECT min(%s) mn, max(%s) mx FROM %s WHERE %s IS NOT NULL) a WHERE a.mn IS NOT NULL LIMIT 10001) a) s '
						 'LEFT JOIN (SELECT %s k, c FROM %s) r ON r.k = s.k) d', step, k, k, c, k, k, c);
		ELSE
			mx := format('(SELECT %s k, c FROM %s WHERE %s IS NOT NULL) d', k, c, k);
		END IF;
		fshape := format('SELECT %s, row_number() OVER (ORDER BY k), %L, ''bucket'', k::text, k::text, (k + interval %L)::text, c FROM %s',
						 no, fname, step, mx);
	END IF;

	IF s ? 'filter' THEN
		sel := coalesce('(' || sel || ') AND ', '') || '(' || (s->>'filter') || ')';
	END IF;
	fown := sel;
	fshape := fshape || miss;
END $fn$;

CREATE FUNCTION sift.facet_sql(tbl regclass, key text, query text, facets jsonb,
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
		RAISE EXCEPTION 'facets must be a non-empty JSON array' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	n := jsonb_array_length(facets);
	FOR i IN 1..n LOOP
		SELECT * INTO p FROM sift.facet_part(facets -> (i - 1), i);
		fkey := fkey || coalesce(p.fkey, ''); fown := fown || coalesce(p.fown, ''); fsrc := fsrc || coalesce(p.fsrc, '');
		faggs := faggs || coalesce(p.faggs, ''); fshape := fshape || p.fshape;
	END LOOP;
	IF query IS NOT NULL THEN
		IF key IS NULL THEN
			RAISE EXCEPTION 'key: the indexed expression of the search is required with a query' USING ERRCODE = 'null_value_not_allowed';
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

-- The counts of the facets of a search: one row per bucket, in the order of the facets. facets is a JSON array whose elements are a column
-- expression (a terms facet) or an object:
--   {"name": "brand", "expr": "brand", "type": "terms", "size": 10, "order": "count", "missing": true, "array": false}
--   {"name": "price", "expr": "price", "type": "range", "edges": [100, 300]}              buckets [,100) [100,300) [300,)
--   {"name": "price", "expr": "price", "type": "histogram", "interval": 50, "offset": 0, "fill": true}
--   {"name": "added", "expr": "added", "type": "date_histogram", "unit": "month", "fill": true}
-- "selected" (values; bucket numbers of a range; the bucket starts of a histogram) and "filter" (a SQL condition) are what the user chose in that
-- facet: they filter every other facet but not this one. key is the expression the sift index is on and query the SiftQL search (NULL: no
-- search), filter a SQL condition for every facet, hits adds the row (facet NULL, kind 'hits') counting what every selection leaves.
-- kind: value (terms), bucket, other (the values past size: lo is how many, count the sum of their counts), missing (NULL values) and hits.
CREATE FUNCTION sift.facet_counts(tbl regclass, key text, query text, facets jsonb,
								  filter text DEFAULT NULL, hits bool DEFAULT false,
								  OUT facet text, OUT kind text, OUT value text, OUT lo text, OUT hi text, OUT count int8)
	RETURNS SETOF record LANGUAGE plpgsql STABLE AS $fn$
BEGIN
	RETURN QUERY EXECUTE sift.facet_sql(tbl, key, query, facets, filter, hits);
END $fn$;
