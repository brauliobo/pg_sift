-- Runner of the plan-choice gate (scripts/plangate.sh). Needs test/col/run.sql (header(), materialize(), compare(), median()).
-- Each shape of shapes.sql runs three ways in one session: native (sift.enable_agg off: the plan PostgreSQL has without sift), chosen (the
-- planner's own choice) and scan (the sequential, index and bitmap scans off, so that Custom Scan (SiftAgg...) runs when the query is one it can).
-- It checks, and records in plangate.results:
--   * the results: equal across the paths by the contract of coldiff.compare (text for every type, floats within 1e-12 + 1e-9 * max);
--   * the plan chosen against the plan of the row of the shape in test/plan/bands.tsv (lane, shape, PLAN, LO, HI): scan, normal or either;
--   * the cost ratio (the planner's total cost of the scan path, the chosen plan when it is one, over the native one) against its band LO HI, so that a drift is seen
--     before it flips a choice (no band, or `-`, when the query has no scan path);
--   * the tag `-- parallel: yes`: under the lane stock4t, the plan of PostgreSQL has a Gather (the shape is one of the parallel scans the lane is for)
--   * the node: a chosen scan has a Custom Plan Provider that the tag `-- node: RE` matches, and no Seq Scan of a relation that one of its Sift nodes reads (the heap hidden beside the
--     scan), nor a node that a tag `-- forbid: NODE on RELATION` names; EXPLAIN and EXPLAIN (VERBOSE) of each path with a Sift node plan without an error
--   * a known gap: the band plan `gap:S113-NN` expects the scan and holds the plan, node, forbid, explain and verbose problems of the shape as known (the column known), not as failures; the
--     shape fails once it has none (gap closed: the band takes the plan, the gap goes). `-- gap: S113-NN` on a shape is the gap --update records for a lane that still has one
--   * the shapes of an area (scripts/plangate.sh --slice): the tag `-- area: A B` of a shape lists the areas it covers (star, clickbench, coexist, jsonb, table, bm25, gpu, bench, smoke)
--   * the times (median of N warm runs): the scan against native. Not a failure of this file: lose is a column the script reads.
\set ON_ERROR_STOP on
CREATE SCHEMA plangate;
CREATE TABLE plangate.results (
	name text PRIMARY KEY, shape text, plan text, chosen text, ratio float8, lo float8, hi float8, native_ms float8, scan_ms float8,
	verdict text,               -- the scan against native: win, tie, lose (more than 1 ms and 20% slower); miss: not chosen, but wins on both counts
	workers int,                -- the most workers a node (Gather, Gather Merge) plans, of the plan chosen: what the parallel lanes (stock4t, tuned) let it use
	native_workers int,         -- the same of the plan PostgreSQL has without sift
	problems text[] NOT NULL DEFAULT '{}',
	gap_problems text[] NOT NULL DEFAULT '{}',   -- the problems a gap holds: the plan, node, forbid, explain and verbose problems against the scan expected
	known text[] NOT NULL DEFAULT '{}',          -- of those, the ones the gap of the band holds (not failures)
	gap text,                                    -- the gap the shape names (`-- gap: S113-NN`), or the one of its band
	coexist boolean NOT NULL DEFAULT false,      -- the shape is of the area coexist: PostgreSQL's plan may be its answer
	nodes text);                                 -- the Custom Plan Providers of the plan chosen
-- the expectation of a shape in a lane (test/plan/bands.tsv, bands-f1.tsv for Format 1; the lane in the setting plangate.lane): its plan and its band.
-- A band is a factor of 2 around the ratio at 30000 rows, the size of the full CI tier: the ratio falls with the rows (plangate.sh --rows), so another size has bands of its own, the lane `LANE@ROWS`
CREATE TABLE plangate.bands (lane text, name text, plan text, lo text, hi text);

CREATE FUNCTION plangate.path(p text) RETURNS void LANGUAGE sql AS $$
	SELECT set_config('sift.enable_agg', (p <> 'native')::text, false), set_config(s, (p <> 'scan')::text, false)
	FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s $$;

-- whether the plan has a Custom Scan of the family Sift (SiftAgg of Format 1, SiftAggV6 and its variants of Format 2.0, SiftHybridAgg, SiftScan), its total cost, the most workers a node
-- of it plans, the plan without its costs (two paths that plan the same nodes run the same plan), its Custom Plan Providers and the plan itself. Before PostgreSQL 18 a disabled node
-- adds 1e10 to the cost: taken off. An EXPLAIN that fails gives its error and NULL for the rest (a Sift node that PostgreSQL cannot print)
CREATE FUNCTION plangate.cost(q text, OUT siftagg boolean, OUT cost float8, OUT workers int, OUT plan text, OUT nodes text, OUT j jsonb, OUT err text) LANGUAGE plpgsql AS $$
DECLARE c float8;
BEGIN
	BEGIN
		EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	EXCEPTION WHEN OTHERS THEN err := SQLERRM; RETURN;
	END;
	c := (j -> 0 -> 'Plan' ->> 'Total Cost')::float8;
	nodes := (SELECT string_agg(DISTINCT p #>> '{}', ' ' ORDER BY p #>> '{}') FROM jsonb_path_query(j, 'lax $.**."Custom Plan Provider"') p);
	siftagg := coalesce(nodes ~ '(^| )Sift(Hybrid)?Agg|(^| )SiftScan', false);
	cost := c - 1e10 * floor(c / 1e10);
	workers := coalesce((SELECT max(m[1]::int) FROM regexp_matches(j::text, '"Workers Planned": (\d+)', 'g') m), 0);
	plan := regexp_replace(j::text, '"(Startup|Total) Cost": [^,}]*', '', 'g');
END $$;

-- the error of EXPLAIN (VERBOSE) of the query under the settings of the session, NULL when it plans: a Sift node whose target list PostgreSQL cannot deparse fails there (and under
-- auto_explain.log_verbose fails the query)
CREATE FUNCTION plangate.verbose(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) ' || q;
	RETURN NULL;
EXCEPTION WHEN OTHERS THEN RETURN SQLERRM;
END $$;

-- the problems of the nodes of a plan with a Sift node (j: EXPLAIN (FORMAT JSON)): the Custom Plan Providers against the tag `-- node: RE`, a Seq Scan of a relation that a Sift node of the
-- plan reads (the heap read beside the scan), and the nodes of the tags `-- forbid: NODE on RELATION`
CREATE FUNCTION plangate.node_problems(txt text, j jsonb, nodes text) RETURNS text[] LANGUAGE sql AS $$
	SELECT array_remove(ARRAY[
		(SELECT format('node: the plan has %s, not /%s/', nodes, m[1]) FROM regexp_match(txt, '(?n)^-- node: (.*?)\s*$') m WHERE m IS NOT NULL AND nodes !~ m[1])]
		|| ARRAY(SELECT DISTINCT format('forbid: Seq Scan on %s, which the Sift node reads', r #>> '{}')
				 FROM jsonb_path_query(j, 'lax $.** ? (@."Node Type" == "Seq Scan")."Relation Name"') r
				 WHERE r IN (SELECT jsonb_path_query(j, 'lax $.** ? (@."Custom Plan Provider" like_regex "^Sift")."Relation Name"')))
		|| ARRAY(SELECT format('forbid: %s on %s', m[1], m[2]) FROM regexp_matches(txt, '(?n)^-- forbid: (.+?) on (\S+)\s*$', 'g') m
				 WHERE jsonb_path_exists(j, 'lax $.** ? (@."Node Type" == $n && @."Relation Name" == $r)', jsonb_build_object('n', m[1], 'r', m[2]))), NULL) $$;

CREATE FUNCTION plangate.ms(q text, runs int) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz; times float8[] := '{}'; r record;
BEGIN
	FOR i IN 1..runs LOOP
		t0 := clock_timestamp();
		FOR r IN EXECUTE q LOOP NULL; END LOOP;
		times := times || extract(epoch FROM clock_timestamp() - t0) * 1000;
	END LOOP;
	RETURN coldiff.median(times);
END $$;

-- one shape (its text: the tags, then the query) into plangate.results
CREATE FUNCTION plangate.run(txt text, runs int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	h jsonb := coldiff.header(txt); q text := h ->> 'body'; nm text := h ->> 'name'; plan text := 'new'; band text[] := ARRAY['-', NULL];
	keycols int[] := CASE WHEN h ->> 'key' = '-' THEN NULL WHEN coalesce(h ->> 'key', '') = '' THEN '{}'::int[] ELSE string_to_array(h ->> 'key', ' ')::int[] END;
	lane_band plangate.bands; anyband boolean := false; problems text[] := '{}'; chosen text; native record; sc record; ch record; err text; diff text; native_ms float8; scan_ms float8; ratio float8; verdict text;
	nruns int := CASE WHEN runs > 0 THEN greatest(runs, coalesce((h ->> 'runs')::int, 0)) ELSE 0 END; ran_chosen boolean := false;
	gap text; tag text := substring(txt FROM '(?n)^-- gap: (S113-\d+)\s*$'); gp text[] := '{}'; known text[] := '{}'; ve text;
BEGIN
	SELECT * INTO lane_band FROM plangate.bands WHERE lane = current_setting('plangate.lane', true) AND name = nm;
	IF FOUND THEN
		anyband := lane_band.lo = 'any';	-- a scan path that exists at some sizes only (no band to hold, none to miss)
		plan := lane_band.plan; band := CASE WHEN anyband THEN ARRAY['-', NULL] ELSE ARRAY[lane_band.lo, nullif(lane_band.hi, '-')] END;
	ELSIF current_setting('plangate.lane', true) LIKE '%@%' THEN
		anyband := true; plan := 'either';	-- a size with no bands recorded (plangate.sh --rows): the results and the loses are checked
	END IF;
	IF band[1] = 'any' THEN anyband := true; band := ARRAY['-', NULL]; END IF;	-- the tag `normal any`: a scan path that exists on some minors of the server only
	IF plan LIKE 'gap:%' THEN gap := substr(plan, 5); plan := 'scan'; END IF;
	PERFORM plangate.path('native');
	native := plangate.cost(q);
	IF txt ~ E'(?m)^-- parallel: yes' AND current_setting('plangate.lane', true) LIKE 'stock4t%' AND native.workers < 1 THEN
		problems := problems || 'parallel: PostgreSQL plans no worker for it under stock-4t (the fact table is under min_parallel_table_scan_size?)'::text;
	END IF;
	err := coldiff.materialize(q, 'pg_native');
	IF err IS NOT NULL THEN problems := problems || ('native: ' || err); END IF;
	IF nruns > 0 AND err IS NULL THEN native_ms := plangate.ms(q, nruns); END IF;

	PERFORM plangate.path('chosen');
	ch := plangate.cost(q);
	chosen := CASE WHEN ch.err IS NOT NULL THEN 'error' WHEN ch.siftagg THEN 'scan' ELSE 'normal' END;
	IF ch.err IS NOT NULL THEN
		gp := ARRAY['explain: chosen: ' || ch.err];
	ELSIF ch.siftagg THEN
		gp := plangate.node_problems(txt, ch.j, ch.nodes);
		ve := plangate.verbose(q);
		IF ve IS NOT NULL THEN gp := gp || ('verbose: chosen: ' || ve); END IF;
	ELSE
		gp := ARRAY[format('plan: chosen %s, expected scan', chosen)];
	END IF;
	IF err IS NULL AND ch.siftagg IS DISTINCT FROM false THEN
		err := coldiff.materialize(q, 'pg_chosen');
		diff := CASE WHEN err IS NULL THEN coldiff.compare('pg_native', 'pg_chosen', keycols, 1e-9, 1e-12) ELSE err END;
		IF diff IS NOT NULL THEN problems := problems || ('result chosen: ' || diff); END IF;
		ran_chosen := true;
	END IF;

	PERFORM plangate.path('scan');
	sc := plangate.cost(q);
	IF sc.err IS NOT NULL AND ch.err IS NULL THEN gp := gp || ('explain: scan: ' || sc.err); END IF;
	IF sc.siftagg AND NOT (ch.siftagg AND sc.plan = ch.plan) THEN
		ve := plangate.verbose(q);
		IF ve IS NOT NULL THEN gp := gp || ('verbose: scan: ' || ve); END IF;
	END IF;
	IF sc.siftagg AND native.cost > 0 THEN
		ratio := CASE WHEN ch.siftagg THEN ch.cost ELSE sc.cost END / native.cost;
		-- the forced scan that plans the nodes of the chosen one returns its rows, its error and its difference: they are those of the chosen run
		IF NOT (ran_chosen AND sc.plan = ch.plan) THEN
			err := coldiff.materialize(q, 'pg_scan');
			diff := CASE WHEN err IS NULL THEN coldiff.compare('pg_native', 'pg_scan', keycols, 1e-9, 1e-12) ELSE err END;
		END IF;
		IF diff IS NOT NULL THEN problems := problems || ('result scan: ' || diff); END IF;
		-- the time of the scan is the time of the plan the planner takes when that is a scan: the one forced with the other scans off can be another plan
		IF nruns > 0 AND err IS NULL THEN PERFORM plangate.path(CASE WHEN ch.siftagg THEN 'chosen' ELSE 'scan' END); scan_ms := plangate.ms(q, nruns); END IF;
	END IF;
	PERFORM plangate.path('chosen');

	IF NOT sc.siftagg THEN gp := gp || 'plan: the scan path does not exist'::text; END IF;
	IF gap IS NOT NULL THEN
		known := gp;
		IF known = '{}' THEN problems := problems || format('gap closed: %s no longer holds here (chosen %s): scripts/plangate.sh --update records the plan; remove the gap', gap, chosen); END IF;
	ELSE
		IF plan = 'new' THEN problems := problems || format('plan: no expectation yet (chosen %s; scripts/plangate.sh --update records it)', chosen);
		ELSIF plan <> 'either' AND plan <> chosen THEN problems := problems || format('plan: chosen %s, expected %s', chosen, plan); END IF;
		IF plan = 'scan' AND ratio IS NULL THEN problems := problems || 'plan: the scan path does not exist'::text; END IF;
		-- the nodes of a chosen scan and EXPLAIN (VERBOSE) of every Sift path hold without a gap
		problems := problems || ARRAY(SELECT p FROM unnest(gp) p WHERE p ~ '^(node|forbid|verbose|explain):');
	END IF;
	IF anyband THEN NULL;
	ELSIF ratio IS NOT NULL AND coalesce(band[1], '-') = '-' THEN problems := problems || format('ratio: %s has no band', round(ratio::numeric, 3));
	ELSIF ratio IS NOT NULL AND NOT ratio BETWEEN band[1]::float8 AND band[2]::float8 THEN problems := problems || format('ratio: %s outside %s %s', round(ratio::numeric, 3), band[1], band[2]);
	ELSIF ratio IS NULL AND coalesce(band[1], '-') <> '-' THEN problems := problems || 'ratio: the scan path does not exist'::text;
	END IF;
	IF native_ms IS NOT NULL THEN
		verdict := CASE WHEN scan_ms IS NULL THEN 'tie'
						WHEN scan_ms > native_ms + 1 AND scan_ms > native_ms * 1.2 THEN CASE WHEN chosen = 'scan' THEN 'lose' ELSE 'tie' END
						WHEN native_ms > scan_ms + 1 AND native_ms > scan_ms * 1.2 THEN CASE WHEN chosen = 'scan' THEN 'win' ELSE 'miss' END
						ELSE 'tie' END;
	END IF;
	INSERT INTO plangate.results VALUES (nm, h ->> 'shape', coalesce('gap:' || gap, plan), chosen, ratio, nullif(band[1], '-')::float8, band[2]::float8, native_ms, scan_ms, verdict, ch.workers,
		native.workers, problems, gp, known, coalesce(tag, gap), txt ~ '(?n)^-- area: .*\mcoexist\M', ch.nodes);
	DROP TABLE IF EXISTS pg_temp.pg_native, pg_temp.pg_chosen, pg_temp.pg_scan;
END $$;

-- every shape of a file whose name matches `pattern` and that has one of the `areas` (comma separated, none: every shape) in its `-- area:` line: a shape starts at its `-- name:` line.
-- Of those, the session `job` of `jobs` runs every jobs-th one from its own (scripts/plangate.sh --jobs: the sessions together run each shape once)
CREATE FUNCTION plangate.run_all(shapes text, runs int, pattern text DEFAULT '', areas text DEFAULT '', job int DEFAULT 0, jobs int DEFAULT 1) RETURNS void LANGUAGE sql AS $$
	SELECT plangate.run(s, runs) FROM (
		SELECT s, row_number() OVER (ORDER BY o) - 1 AS k FROM regexp_split_to_table(shapes, E'\n(?=-- name: )') WITH ORDINALITY t(s, o)
		WHERE substring(s FROM '^-- name: (\S+)') ~ pattern
		  AND (areas = '' OR string_to_array(substring(s FROM '(?n)^-- area: (.*)$'), ' ') && string_to_array(areas, ','))) x
	WHERE k % jobs = job ORDER BY k $$;
