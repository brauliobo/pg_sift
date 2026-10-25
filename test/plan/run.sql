-- Runner of the plan-choice gate (scripts/plangate.sh). Needs test/col/run.sql (header(), materialize(), compare(), median()).
-- Each shape of shapes.sql runs three ways in one session: native (sift.enable_agg off: the plan PostgreSQL has without sift), chosen (the
-- planner's own choice) and scan (the sequential, index and bitmap scans off, so that Custom Scan (SiftAgg) runs when the query is one it can).
-- It checks, and records in plangate.results:
--   * the results: equal across the paths by the contract of coldiff.compare (text for every type, floats within 1e-12 + 1e-9 * max);
--   * the plan chosen against the plan of the tag `-- expect: PLAN [LO HI]`: scan, normal or either;
--   * the cost ratio (the planner's total cost of the scan path, the chosen plan when it is one, over the native one) against its band LO HI, so that a drift is seen
--     before it flips a choice (no band, or `-`, when the query has no scan path);
--   * the times (median of N warm runs): the scan against native. Not a failure of this file: lose is a column the script reads.
\set ON_ERROR_STOP on
CREATE SCHEMA plangate;
CREATE TABLE plangate.results (
	name text PRIMARY KEY, shape text, plan text, chosen text, ratio float8, lo float8, hi float8, native_ms float8, scan_ms float8,
	verdict text,               -- the scan against native: win, tie, lose (more than 1 ms and 20% slower); miss: not chosen, but wins on both counts
	problems text[] NOT NULL DEFAULT '{}');
-- the expectation of a shape in a lane other than stock (test/plan/bands.tsv, the lane in the setting plangate.lane): its plan and band replace the tag of the shape.
-- A band is a factor of 2 around the ratio at 30000 rows; where the ratio moves with the rows (20000 to 50000, the size of the full CI tier), the smallest and largest of the three divided and multiplied by 1.3
CREATE TABLE plangate.bands (lane text, name text, plan text, lo text, hi text);

CREATE FUNCTION plangate.path(p text) RETURNS void LANGUAGE sql AS $$
	SELECT set_config('sift.enable_agg', (p <> 'native')::text, false), set_config(s, (p <> 'scan')::text, false)
	FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s $$;

-- whether the plan has Custom Scan (SiftAgg), and its total cost. Before PostgreSQL 18 a disabled node adds 1e10 to the cost: taken off.
CREATE FUNCTION plangate.cost(q text, OUT siftagg boolean, OUT cost float8) LANGUAGE plpgsql AS $$
DECLARE j jsonb; c float8;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	c := (j -> 0 -> 'Plan' ->> 'Total Cost')::float8;
	siftagg := j::text LIKE '%"SiftAgg"%';
	cost := c - 1e10 * floor(c / 1e10);
END $$;

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
	h jsonb := coldiff.header(txt); q text := h ->> 'body'; nm text := h ->> 'name'; tags text[] := string_to_array(coalesce(h ->> 'expect', 'untagged -'), ' '); plan text := tags[1]; band text[] := tags[2:3];
	keycols int[] := CASE WHEN h ->> 'key' = '-' THEN NULL WHEN coalesce(h ->> 'key', '') = '' THEN '{}'::int[] ELSE string_to_array(h ->> 'key', ' ')::int[] END;
	lane_band plangate.bands; problems text[] := '{}'; chosen text; native record; sc record; ch record; err text; diff text; native_ms float8; scan_ms float8; ratio float8; verdict text;
BEGIN
	SELECT * INTO lane_band FROM plangate.bands WHERE lane = current_setting('plangate.lane', true) AND name = nm;
	IF FOUND THEN plan := lane_band.plan; band := ARRAY[lane_band.lo, nullif(lane_band.hi, '-')]; END IF;
	PERFORM plangate.path('native');
	native := plangate.cost(q);
	err := coldiff.materialize(q, 'pg_native');
	IF err IS NOT NULL THEN problems := problems || ('native: ' || err); END IF;
	IF runs > 0 AND err IS NULL THEN native_ms := plangate.ms(q, runs); END IF;

	PERFORM plangate.path('chosen');
	ch := plangate.cost(q);
	chosen := CASE WHEN ch.siftagg THEN 'scan' ELSE 'normal' END;
	IF err IS NULL AND ch.siftagg THEN
		err := coldiff.materialize(q, 'pg_chosen');
		diff := CASE WHEN err IS NULL THEN coldiff.compare('pg_native', 'pg_chosen', keycols, 1e-9, 1e-12) ELSE err END;
		IF diff IS NOT NULL THEN problems := problems || ('result chosen: ' || diff); END IF;
	END IF;

	PERFORM plangate.path('scan');
	sc := plangate.cost(q);
	IF sc.siftagg AND native.cost > 0 THEN
		ratio := CASE WHEN ch.siftagg THEN ch.cost ELSE sc.cost END / native.cost;
		err := coldiff.materialize(q, 'pg_scan');
		diff := CASE WHEN err IS NULL THEN coldiff.compare('pg_native', 'pg_scan', keycols, 1e-9, 1e-12) ELSE err END;
		IF diff IS NOT NULL THEN problems := problems || ('result scan: ' || diff); END IF;
		IF runs > 0 AND err IS NULL THEN scan_ms := plangate.ms(q, runs); END IF;
	END IF;
	PERFORM plangate.path('chosen');

	IF plan = 'new' THEN problems := problems || format('plan: no expectation yet (chosen %s; scripts/plangate.sh --update records it)', chosen);
	ELSIF plan <> 'either' AND plan <> chosen THEN problems := problems || format('plan: chosen %s, expected %s', chosen, plan); END IF;
	IF plan = 'scan' AND ratio IS NULL THEN problems := problems || 'plan: the scan path does not exist'::text; END IF;
	IF ratio IS NOT NULL AND coalesce(band[1], '-') = '-' THEN problems := problems || format('ratio: %s has no band', round(ratio::numeric, 3));
	ELSIF ratio IS NOT NULL AND NOT ratio BETWEEN band[1]::float8 AND band[2]::float8 THEN problems := problems || format('ratio: %s outside %s %s', round(ratio::numeric, 3), band[1], band[2]);
	ELSIF ratio IS NULL AND coalesce(band[1], '-') <> '-' THEN problems := problems || 'ratio: the scan path does not exist'::text;
	END IF;
	IF native_ms IS NOT NULL THEN
		verdict := CASE WHEN scan_ms IS NULL THEN 'tie'
						WHEN scan_ms > native_ms + 1 AND scan_ms > native_ms * 1.2 THEN CASE WHEN chosen = 'scan' THEN 'lose' ELSE 'tie' END
						WHEN native_ms > scan_ms + 1 AND native_ms > scan_ms * 1.2 THEN CASE WHEN chosen = 'scan' THEN 'win' ELSE 'miss' END
						ELSE 'tie' END;
	END IF;
	INSERT INTO plangate.results VALUES (nm, h ->> 'shape', plan, chosen, ratio, nullif(band[1], '-')::float8, band[2]::float8, native_ms, scan_ms, verdict, problems);
	DROP TABLE IF EXISTS pg_temp.pg_native, pg_temp.pg_chosen, pg_temp.pg_scan;
END $$;

-- every shape of a file whose name matches `pattern`: a shape starts at its `-- name:` line
CREATE FUNCTION plangate.run_all(shapes text, runs int, pattern text DEFAULT '') RETURNS void LANGUAGE sql AS $$
	SELECT plangate.run(s, runs) FROM regexp_split_to_table(shapes, E'\n(?=-- name: )') s WHERE s ~ ('^-- name: ' || pattern) $$;

-- the `-- expect:` line each shape would have from this run: its plan (the chosen one where it is new) and a band of a factor of 2 either way, in 2 significant digits
CREATE VIEW plangate.suggestions AS
	SELECT name, concat_ws(' ', CASE WHEN plan = 'new' THEN chosen ELSE plan END,
		coalesce(round(ratio::numeric / 2, 1 - floor(log(ratio::numeric / 2))::int) || ' ' || round(ratio::numeric * 2, 1 - floor(log(ratio::numeric * 2))::int), '-')) AS expect
	FROM plangate.results;
