-- Differential runner of scripts/coldiff.sh (docs/AGG.md, "Differential suite"): every corpus query is run in one session with the
-- aggregate pushdown off (the reference, the plan PostgreSQL has without sift) and in the variants below, the result of each run is
-- materialized in a temporary table and the tables are compared in the database.
--
-- Contract of the comparison (the one the documentation states):
--   * columns (names, types and typmods) and the number of rows must be equal; rows are matched by position, after the sort of
--     `-- key:` (columns, then every column as text) or, with `-- key: -`, as the query itself ordered them (a total ORDER BY);
--   * integer, date, text, boolean, numeric, json(b), array and every other type: equal as text (a numeric keeps its scale);
--   * float4 and float8: |x - y| <= 1e-12 + 1e-9 * max(|x|, |y|); NaN equals NaN, an infinity only itself, -0 equals 0, NULL only NULL.
--     Parallel workers and SIMD lanes add in another order, so sums and averages of floats differ in the last digits;
--   * a query that raises an error must raise one with the same SQLSTATE in every variant (the message may name another value).
--
-- Variants: off (reference: sift.enable_agg off, no parallel workers), off-par, on, on-par, on-scalar, on-par-scalar. "on" in the name:
-- sift.enable_agg = on; "par": max_parallel_workers_per_gather 2 with free parallel costs; "scalar": sift.simd = scalar (else auto).
\set ON_ERROR_STOP on
CREATE SCHEMA IF NOT EXISTS coldiff;
DROP TABLE IF EXISTS coldiff.results;
CREATE TABLE coldiff.results (
	id serial PRIMARY KEY, scenario text NOT NULL, name text NOT NULL, shape text, expect text, variant text NOT NULL,
	ok boolean NOT NULL,        -- the result (or the error) equals the reference's
	used boolean,               -- the plan has Custom Scan (SiftAgg)
	gather boolean,             -- the plan has a Gather
	shape_ok boolean,           -- the expectation of the query holds (NULL: no expectation or not checked for this variant)
	ref_ms float8, ms float8,   -- the reference and the variant
	detail text);

-- float comparison of the contract: relative 1e-9 plus absolute 1e-12 (halved before the subtraction so that 1e308 - -1e308 cannot overflow)
CREATE FUNCTION coldiff.feq(x float8, y float8, rel float8, abs_tol float8) RETURNS boolean IMMUTABLE PARALLEL SAFE LANGUAGE sql AS $$
	SELECT CASE WHEN x IS NULL OR y IS NULL THEN x IS NULL AND y IS NULL
				WHEN x = y THEN true
				WHEN x = 'NaN' OR y = 'NaN' OR abs(x) = 'Infinity' OR abs(y) = 'Infinity' THEN false
				ELSE abs(x / 2 - y / 2) <= abs_tol / 2 + rel * greatest(abs(x), abs(y)) / 2 END $$;

-- The tags of a corpus file. The header is the leading `--` lines: `-- tag: value` for the tags below (any other line is a title),
-- then the body, or, in the files of bench/sql/analytics, the `--@ pg` section.
--   name shape expect(agg|plain|any) scenario(yes) sets(K10 K400 ...) key(1 2 | -) guc(name = value, repeatable) prepare((types))
--   args(values of EXECUTE) requires(a SELECT that must return true, repeatable) wrap(subquery) error(yes) isolation(serializable ...)
CREATE FUNCTION coldiff.header(txt text) RETURNS jsonb IMMUTABLE LANGUAGE plpgsql AS $$
DECLARE
	line text; m text[]; h jsonb := '{}'; sect text; body text[] := '{}'; inhead boolean := true; marked boolean := false;
	tags text[] := ARRAY['sets','key','tol','name','shape','expect','scenario','guc','prepare','args','requires','wrap','error','isolation'];
BEGIN
	FOREACH line IN ARRAY string_to_array(txt, E'\n') LOOP
		IF line ~ '^--@ ' THEN
			sect := btrim(substr(line, 5)); inhead := false; marked := true;
			CONTINUE;
		END IF;
		IF inhead AND line ~ '^--' THEN
			m := regexp_match(line, '^--\s*(\w+):[ \t]*(.*?)\s*$');
			IF m IS NOT NULL AND m[1] = ANY (tags) THEN
				IF m[1] IN ('guc', 'requires') THEN h := jsonb_set(h, ARRAY[m[1]], coalesce(h -> m[1], '[]') || to_jsonb(m[2]));
				ELSE h := h || jsonb_build_object(m[1], m[2]);
				END IF;
			END IF;
			CONTINUE;
		END IF;
		inhead := false;
		IF NOT marked OR sect = 'pg' THEN body := body || line; END IF;
	END LOOP;
	RETURN h || jsonb_build_object('body', btrim(array_to_string(body, E'\n'), E' \n\t;'));
END $$;

-- the isolation level of a file for BEGIN ISOLATION LEVEL
CREATE FUNCTION coldiff.iso(txt text) RETURNS text IMMUTABLE LANGUAGE sql AS $$
	SELECT coalesce(coldiff.header(txt) ->> 'isolation', 'read committed') $$;

-- The macros of bench/sql/analytics/queries (expand() of bench/steps/analytics.py is the reference): {stores} the members of the set
-- of analytics_sets, {sources} the quoted members of the set 'sources', {period(col)} the label of the period, {range(col)} both periods.
CREATE FUNCTION coldiff.expand(q text, setname text) RETURNS text STABLE LANGUAGE plpgsql AS $$
BEGIN
	q := replace(q, '{stores}', (SELECT coalesce(string_agg(member, ',' ORDER BY member::int), '-1') FROM analytics_sets WHERE set_name = setname));
	q := replace(q, '{sources}', (SELECT coalesce(string_agg(quote_literal(member), ',' ORDER BY member), '''''') FROM analytics_sets WHERE set_name = 'sources'));
	q := regexp_replace(q, '\{period\(([^)]*)\)\}', 'CASE WHEN \1 >= ''2024-11-01'' AND \1 <= ''2025-10-31'' THEN ''current'' '
						'WHEN \1 >= ''2023-11-01'' AND \1 <= ''2024-10-31'' THEN ''current-1'' ELSE NULL END', 'g');
	RETURN regexp_replace(q, '\{range\(([^)]*)\)\}', '\1 >= ''2023-11-01'' AND \1 <= ''2025-10-31''', 'g');
END $$;

-- the session settings of a variant, then the extra ones of the file ("name = value")
CREATE FUNCTION coldiff.apply(variant text, gucs text[]) RETURNS void LANGUAGE plpgsql AS $$
DECLARE g text; m text[];
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN variant LIKE 'on%' THEN 'on' ELSE 'off' END, false);
	PERFORM set_config('sift.simd', CASE WHEN variant LIKE '%scalar%' THEN 'scalar' ELSE 'auto' END, false);
	PERFORM set_config('max_parallel_workers_per_gather', CASE WHEN variant LIKE '%par%' THEN '2' ELSE '0' END, false);
	IF variant LIKE '%par%' THEN
		PERFORM set_config('parallel_setup_cost', '0', false), set_config('parallel_tuple_cost', '0', false),
				set_config('min_parallel_table_scan_size', '0', false), set_config('min_parallel_index_scan_size', '0', false);
	END IF;
	FOREACH g IN ARRAY coalesce(gucs, '{}') LOOP
		m := regexp_match(g, '^\s*([\w.]+)\s*(?:=|TO)\s*(.*?)\s*$');
		PERFORM set_config(m[1], m[2], false);
	END LOOP;
END $$;

CREATE FUNCTION coldiff.reset(gucs text[]) RETURNS void LANGUAGE plpgsql AS $$
DECLARE g text;
BEGIN
	FOREACH g IN ARRAY ARRAY['sift.enable_agg', 'sift.simd', 'max_parallel_workers_per_gather', 'parallel_setup_cost', 'parallel_tuple_cost',
							 'min_parallel_table_scan_size', 'min_parallel_index_scan_size'] || coalesce(gucs, '{}') LOOP
		EXECUTE 'RESET ' || (regexp_match(g, '^\s*([\w.]+)'))[1];
	END LOOP;
END $$;

-- runs a statement into the temporary table tbl: NULL, or the SQLSTATE and the message of its error
CREATE FUNCTION coldiff.materialize(stmt text, tbl text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('DROP TABLE IF EXISTS pg_temp.%I', tbl);
	EXECUTE format('CREATE TEMP TABLE %I AS %s', tbl, stmt);
	RETURN NULL;
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;

CREATE FUNCTION coldiff.explain(stmt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || stmt LOOP plan := plan || l || E'\n'; END LOOP;
	RETURN plan;
EXCEPTION WHEN OTHERS THEN
	RETURN 'EXPLAIN ERROR ' || SQLSTATE || ': ' || SQLERRM;
END $$;

-- The go/no-go targets of --bench on a query run: the K4000 queries that must push down (expect agg, and the S shapes of the analytics
-- corpus) at least 20 times faster than the reference, the K10 and K400 ones that push down not slower than 0.95
CREATE FUNCTION coldiff.bench_target(name text, expect text, used boolean, ref_ms float8, ms float8) RETURNS text IMMUTABLE LANGUAGE sql AS $$
	SELECT CASE WHEN name LIKE '%@K4000' AND (expect = 'agg' OR name ~ '^S[0-9]')
				THEN CASE WHEN NOT used THEN 'NOT PUSHED DOWN' WHEN ref_ms / nullif(ms, 0) >= 20 THEN 'ok >= 20x' ELSE 'MISSED: 20x' END
				WHEN used AND (name LIKE '%@K10' OR name LIKE '%@K400')
				THEN CASE WHEN ref_ms / nullif(ms, 0) >= 0.95 THEN 'ok no regression' ELSE 'REGRESSION' END
				ELSE '' END $$;

CREATE FUNCTION coldiff.median(v float8[]) RETURNS float8 IMMUTABLE LANGUAGE sql AS $$
	SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM unnest(v) x $$;

-- The two temporary tables, NULL when equal, else what differs. keycols: NULL = by position as the query ordered them, else the
-- columns (1-based) to sort both by first, then every column as text.
CREATE FUNCTION coldiff.compare(ref text, got text, keycols int[], rel float8, abs_tol float8) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	sig_ref text; sig_got text; ord text; cond text; r record; shown text := ''; nref bigint; ngot bigint; sig_sql text :=
		$s$SELECT string_agg(a.attname || ' ' || format_type(a.atttypid, a.atttypmod), ', ' ORDER BY a.attnum)
		   FROM pg_attribute a WHERE a.attrelid = to_regclass('pg_temp.' || quote_ident($1)) AND a.attnum > 0 AND NOT a.attisdropped$s$;
BEGIN
	EXECUTE sig_sql INTO sig_ref USING ref;
	EXECUTE sig_sql INTO sig_got USING got;
	IF sig_ref IS DISTINCT FROM sig_got THEN
		RETURN format('columns differ: off (%s) on (%s)', sig_ref, sig_got);
	END IF;
	SELECT CASE WHEN keycols IS NULL THEN 'ctid'
				ELSE string_agg(format('(%I)::text COLLATE "C"', a.attname), ', ' ORDER BY coalesce(array_position(keycols, a.attnum::int), 1000 + a.attnum)) END,
		   string_agg(CASE WHEN a.atttypid IN ('float4'::regtype, 'float8'::regtype)
						   THEN format('coldiff.feq(coldiff_a.%1$I::float8, coldiff_b.%1$I::float8, %2$L, %3$L)', a.attname, rel, abs_tol)
						   ELSE format('(coldiff_a.%1$I)::text IS NOT DISTINCT FROM (coldiff_b.%1$I)::text', a.attname) END, ' AND ' ORDER BY a.attnum)
	INTO ord, cond
	FROM pg_attribute a WHERE a.attrelid = to_regclass('pg_temp.' || quote_ident(ref)) AND a.attnum > 0 AND NOT a.attisdropped;
	FOR r IN EXECUTE format($q$
		SELECT coalesce(coldiff_a.coldiff_rn, coldiff_b.coldiff_rn) AS rn, to_jsonb(coldiff_a) - 'coldiff_rn' AS ref, to_jsonb(coldiff_b) - 'coldiff_rn' AS got
		FROM (SELECT row_number() OVER (ORDER BY %1$s) AS coldiff_rn, * FROM pg_temp.%2$I) coldiff_a
		FULL JOIN (SELECT row_number() OVER (ORDER BY %1$s) AS coldiff_rn, * FROM pg_temp.%3$I) coldiff_b ON coldiff_a.coldiff_rn = coldiff_b.coldiff_rn
		WHERE coldiff_a.coldiff_rn IS NULL OR coldiff_b.coldiff_rn IS NULL OR NOT (%4$s) ORDER BY 1 LIMIT 3$q$, ord, ref, got, cond)
	LOOP
		shown := shown || format(E'\n    row %s\n      off: %s\n      on:  %s', r.rn, left(coalesce(r.ref::text, '<no row>'), 400),
								 left(coalesce(r.got::text, '<no row>'), 400));
	END LOOP;
	IF shown = '' THEN RETURN NULL; END IF;
	EXECUTE format('SELECT count(*) FROM pg_temp.%I', ref) INTO nref;
	EXECUTE format('SELECT count(*) FROM pg_temp.%I', got) INTO ngot;
	RETURN format('%s rows off, %s rows on; first differing rows:%s', nref, ngot, shown);
END $$;

-- Self-test support: perturbs the temporary table of an "on" run the way a bug of the pushdown would. NULL when the kind does not apply
-- to the result (no row, no column of the type). Kinds: int (+1), float (relative 1e-6: beyond the tolerance), float-tiny (relative
-- 1e-11: within it), text (a suffix), null, rows (the last row is lost), dup (the first row twice), order (rows 1 and 2 swapped: seen
-- only by a query compared by position), type (the type of the first column that has a wider one), cols (an extra column).
CREATE FUNCTION coldiff.mutate(tbl text, kind text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	t text := 'pg_temp.' || quote_ident(tbl); n bigint; c record; target bigint; expr text; sel text; ord text := 'coldiff_rn';
	src text := format('(SELECT row_number() OVER (ORDER BY ctid) AS coldiff_rn, * FROM %s) s', 'pg_temp.' || quote_ident(tbl));
	cols text; wher text := '';
BEGIN
	EXECUTE format('SELECT count(*) FROM %s', t) INTO n;
	IF kind = 'cols' THEN
		EXECUTE format('ALTER TABLE %s ADD COLUMN coldiff_extra int', t);
		RETURN 'extra column';
	ELSIF n = 0 THEN
		RETURN NULL;
	END IF;
	SELECT string_agg(format('%I', attname), ', ' ORDER BY attnum) INTO cols
	FROM pg_attribute WHERE attrelid = t::regclass AND attnum > 0 AND NOT attisdropped;
	IF kind = 'type' THEN
		SELECT a.attname, CASE WHEN a.atttypid IN ('int2'::regtype, 'int4'::regtype) THEN 'bigint' WHEN a.atttypid = 'int8'::regtype THEN 'numeric'
							   WHEN a.atttypid = 'float4'::regtype THEN 'float8' WHEN a.atttypid = 'float8'::regtype THEN 'numeric'
							   WHEN a.atttypid = 'date'::regtype THEN 'timestamp' ELSE 'varchar' END AS wider INTO c
		FROM pg_attribute a WHERE a.attrelid = t::regclass AND a.attnum > 0 AND NOT a.attisdropped
		  AND a.atttypid IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype, 'float4'::regtype, 'float8'::regtype, 'date'::regtype, 'text'::regtype)
		ORDER BY a.attnum LIMIT 1;
		IF c.attname IS NULL THEN RETURN NULL; END IF;
		EXECUTE format('ALTER TABLE %s ALTER COLUMN %I TYPE %s', t, c.attname, c.wider);
		RETURN 'type of ' || c.attname;
	ELSIF kind = 'rows' THEN
		wher := format('WHERE coldiff_rn < %s', n);
	ELSIF kind = 'order' THEN
		EXECUTE format('SELECT count(DISTINCT s::text) FROM (SELECT * FROM %s ORDER BY ctid LIMIT 2) s', t) INTO target;
		IF n < 2 OR target < 2 THEN RETURN NULL; END IF;
		ord := 'CASE coldiff_rn WHEN 1 THEN 2 WHEN 2 THEN 1 ELSE coldiff_rn END';
	ELSIF kind IN ('int', 'float', 'float-tiny', 'text', 'null') THEN
		-- the first column of the kind with a value to perturb, in the first row that has one
		FOR c IN SELECT attname, atttypid, format_type(atttypid, atttypmod) AS typ FROM pg_attribute
				 WHERE attrelid = t::regclass AND attnum > 0 AND NOT attisdropped
				   AND (CASE WHEN kind = 'int' THEN atttypid IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype)
							 WHEN kind IN ('float', 'float-tiny') THEN atttypid IN ('float4'::regtype, 'float8'::regtype)
							 WHEN kind = 'text' THEN atttypid = 'text'::regtype ELSE true END) ORDER BY attnum
		LOOP
			EXECUTE format('SELECT min(coldiff_rn) FROM %s WHERE %I IS NOT NULL %s', src, c.attname,
						   CASE WHEN kind LIKE 'float%' THEN format('AND %I < ''Infinity'' AND %I > ''-Infinity''', c.attname, c.attname) ELSE '' END) INTO target;
			EXIT WHEN target IS NOT NULL;
		END LOOP;
		IF target IS NULL THEN RETURN NULL; END IF;
		expr := CASE kind WHEN 'int' THEN format('(%I + 1)::%s', c.attname, c.typ)
						  WHEN 'float' THEN format('(%1$I + 1e-6 * (abs(%1$I) + 1))::%2$s', c.attname, c.typ)
						  WHEN 'float-tiny' THEN format('(%1$I * (1 + 1e-11))::%2$s', c.attname, c.typ)
						  WHEN 'text' THEN format('%I || ''x''', c.attname)
						  ELSE format('NULL::%s', c.typ) END;
		sel := (SELECT string_agg(CASE WHEN a.attname = c.attname THEN format('CASE WHEN coldiff_rn = %s THEN %s ELSE %I END AS %I', target, expr, a.attname, a.attname)
									   ELSE format('%I', a.attname) END, ', ' ORDER BY a.attnum)
				FROM pg_attribute a WHERE a.attrelid = t::regclass AND a.attnum > 0 AND NOT a.attisdropped);
		cols := sel;
	ELSIF kind <> 'dup' THEN
		RAISE EXCEPTION 'coldiff: unknown mutation %', kind;
	END IF;
	EXECUTE format('CREATE TEMP TABLE coldiff_mut AS SELECT %s FROM %s %s ORDER BY %s', cols, src, wher, ord);
	IF kind = 'dup' THEN
		EXECUTE format('INSERT INTO coldiff_mut SELECT %s FROM %s WHERE coldiff_rn = 1', cols, src);
	END IF;
	EXECUTE format('DROP TABLE %s', t);
	EXECUTE format('ALTER TABLE coldiff_mut RENAME TO %I', tbl);
	RETURN kind;
END $$;

-- One corpus file (txt) in one transaction: the reference, then each variant, compared; rows into coldiff.results. runs > 1: the
-- times of the reference and of "on" are the median of that many extra runs (a warm run each). mutate: coldiff.mutate on the "on*" runs.
CREATE FUNCTION coldiff.run_query(scen text, qname text, txt text, variants text[], runs int DEFAULT 1, mutate text DEFAULT NULL,
								  rel float8 DEFAULT 1e-9, abs_tol float8 DEFAULT 1e-12) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	h jsonb := coldiff.header(txt); gucs text[] := ARRAY(SELECT jsonb_array_elements_text(coalesce(h -> 'guc', '[]')));
	setname text; sets text[]; q text; stmt text; plan text; v text; err_ref text; err_got text; msg text; used boolean; gat boolean;
	shape_ok boolean; t0 timestamptz; ref_ms float8; ms float8; times float8[]; name text; expect text := coalesce(h ->> 'expect', 'any');
	keycols int[]; wanted_error boolean := coalesce(h ->> 'error', '') = 'yes'; req text; ok boolean; i int; mutated text;
	shape text := coalesce(h ->> 'shape', CASE WHEN qname ~ '^[SG][0-9]+$' THEN qname END);
BEGIN
	sets := CASE WHEN coalesce(h ->> 'sets', '-') IN ('-', '') THEN ARRAY[NULL]::text[] ELSE string_to_array(h ->> 'sets', ' ') END;
	keycols := CASE WHEN h ->> 'key' = '-' THEN NULL WHEN coalesce(h ->> 'key', '') = '' THEN '{}'::int[]
					ELSE string_to_array(h ->> 'key', ' ')::int[] END;
	FOR req IN SELECT jsonb_array_elements_text(coalesce(h -> 'requires', '[]')) LOOP
		EXECUTE req INTO ok;
		IF NOT ok THEN
			INSERT INTO coldiff.results (scenario, name, shape, expect, variant, ok, detail)
			VALUES (scen, qname, shape, expect, 'skip', true, 'requires not met: ' || req);
			RETURN;
		END IF;
	END LOOP;
	FOREACH setname IN ARRAY sets LOOP
		name := qname || coalesce('@' || setname, '');
		q := coldiff.expand(h ->> 'body', setname);
		IF h ? 'prepare' THEN
			stmt := format('EXECUTE coldiff_p(%s)', coldiff.expand(coalesce(h ->> 'args', ''), setname));
		ELSIF h ->> 'wrap' = 'subquery' THEN
			stmt := format('SELECT * FROM (%s) coldiff_w', q);
		ELSE
			stmt := q;
		END IF;
		-- the reference
		PERFORM coldiff.apply('off', gucs);
		IF h ? 'prepare' THEN
			DEALLOCATE ALL;
			EXECUTE format('PREPARE coldiff_p %s AS %s', h ->> 'prepare', q);
		END IF;
		t0 := clock_timestamp();
		err_ref := coldiff.materialize(stmt, 'coldiff_ref');
		ref_ms := extract(epoch FROM clock_timestamp() - t0) * 1000;
		IF runs > 1 THEN
			times := '{}';
			FOR i IN 1..runs LOOP
				t0 := clock_timestamp();
				PERFORM coldiff.materialize(stmt, 'coldiff_tmp');
				times := times || extract(epoch FROM clock_timestamp() - t0) * 1000;
			END LOOP;
			ref_ms := coldiff.median(times);
		END IF;
		IF err_ref IS NOT NULL AND NOT wanted_error THEN
			INSERT INTO coldiff.results (scenario, name, shape, expect, variant, ok, ref_ms, detail)
			VALUES (scen, name, shape, expect, 'off', false, ref_ms, 'the reference run failed: ' || err_ref);
			CONTINUE;
		ELSIF err_ref IS NULL AND wanted_error THEN
			INSERT INTO coldiff.results (scenario, name, shape, expect, variant, ok, ref_ms, detail)
			VALUES (scen, name, shape, expect, 'off', false, ref_ms, 'the query is tagged "error: yes" and ran without one');
			CONTINUE;
		END IF;
		-- the variants
		FOREACH v IN ARRAY variants LOOP
			PERFORM coldiff.apply(v, gucs);
			IF h ? 'prepare' THEN
				DEALLOCATE ALL;
				EXECUTE format('PREPARE coldiff_p %s AS %s', h ->> 'prepare', q);
			END IF;
			t0 := clock_timestamp();
			err_got := coldiff.materialize(stmt, 'coldiff_got');
			ms := extract(epoch FROM clock_timestamp() - t0) * 1000;
			IF runs > 1 AND v = 'on' THEN
				times := '{}';
				FOR i IN 1..runs LOOP
					t0 := clock_timestamp();
					PERFORM coldiff.materialize(stmt, 'coldiff_tmp');
					times := times || extract(epoch FROM clock_timestamp() - t0) * 1000;
				END LOOP;
				ms := coldiff.median(times);
			END IF;
			plan := coldiff.explain(stmt);
			used := plan LIKE '%Custom Scan (SiftAgg)%';
			gat := plan LIKE '%Gather%';
			mutated := NULL;
			IF mutate IS NOT NULL AND v LIKE 'on%' AND err_got IS NULL THEN mutated := coldiff.mutate('coldiff_got', mutate); END IF;
			IF err_ref IS NOT NULL OR err_got IS NOT NULL THEN
				msg := CASE WHEN left(err_ref, 5) IS NOT DISTINCT FROM left(err_got, 5) THEN NULL
							ELSE format('error differs: off %s; on %s', coalesce(err_ref, 'no error'), coalesce(err_got, 'no error')) END;
			ELSE
				msg := coldiff.compare('coldiff_ref', 'coldiff_got', keycols, rel, abs_tol);
			END IF;
			shape_ok := CASE WHEN expect = 'agg' AND v = 'on' THEN used WHEN expect = 'plain' AND v LIKE 'on%' THEN NOT used END;
			INSERT INTO coldiff.results (scenario, name, shape, expect, variant, ok, used, gather, shape_ok, ref_ms, ms, detail)
			VALUES (scen, name, shape, expect, v, msg IS NULL, used, gat, shape_ok, ref_ms, ms,
					nullif(concat(coalesce(msg, CASE WHEN err_ref IS NOT NULL THEN 'both raise ' || left(err_ref, 80) END),
								  CASE WHEN mutated IS NOT NULL THEN ' [mutated: ' || mutated || ']' END), ''));
			PERFORM coldiff.reset(gucs);
		END LOOP;
		PERFORM coldiff.reset(gucs);
		IF h ? 'prepare' THEN DEALLOCATE ALL; END IF;
		DROP TABLE IF EXISTS pg_temp.coldiff_ref, pg_temp.coldiff_got, pg_temp.coldiff_tmp;
	END LOOP;
END $$;
