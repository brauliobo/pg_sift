-- The estimate calibration of the plan-choice gate (scripts/plangate.sh --calibrate; docs/AGG.md "Plan-estimate calibration"). Needs run.sql (header()).
-- Every shape of calibrate.tsv is planned once for each way the planner can go (EXPLAIN only): the scan (Custom Scan (SiftAgg), the others off), a sequential
-- scan, a b-tree index scan, a bitmap scan, PostgreSQL's own choice without sift and the planner's own choice with it. Plans with the same shape (the EXPLAIN text
-- without the costs) are one plan. The script measures the instructions of each distinct plan and compares the ranking the estimates give with the one they have.
\set ON_ERROR_STOP on
CREATE TABLE plangate.cal_shapes (name text PRIMARY KEY, class text NOT NULL, body text NOT NULL);
CREATE TABLE plangate.cal_plans (name text NOT NULL, variant text NOT NULL, cost float8 NOT NULL, sig text NOT NULL, sift boolean NOT NULL, PRIMARY KEY (name, variant));
CREATE TABLE plangate.cal_measured (name text NOT NULL, sig text NOT NULL, instr float8 NOT NULL, PRIMARY KEY (name, sig));

-- the settings of a way: scan, seq, index, bitmap, native (no sift), chosen (everything on)
CREATE FUNCTION plangate.cal_set(v text) RETURNS void LANGUAGE sql AS $$
	SELECT set_config('sift.enable_agg', (v IN ('scan', 'chosen'))::text, false),
		   set_config('enable_seqscan', (v IN ('seq', 'native', 'chosen'))::text, false),
		   set_config('enable_indexscan', (v IN ('index', 'native', 'chosen'))::text, false),
		   set_config('enable_indexonlyscan', (v IN ('index', 'native', 'chosen'))::text, false),
		   set_config('enable_bitmapscan', (v IN ('bitmap', 'native', 'chosen'))::text, false) $$;

CREATE FUNCTION plangate.cal_load(shapes text, tsv text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE n int := 0; r record; s text;
BEGIN
	FOR r IN SELECT split_part(l, E'\t', 1) AS name, split_part(l, E'\t', 2) AS class FROM regexp_split_to_table(tsv, E'\n') l WHERE l <> '' AND l !~ '^#' LOOP
		SELECT x INTO s FROM regexp_split_to_table(shapes, E'\n(?=-- name: )') x WHERE x ~ ('^-- name: ' || r.name || E'\n');
		IF s IS NULL THEN RAISE EXCEPTION 'calibrate.tsv: no shape %', r.name; END IF;
		INSERT INTO plangate.cal_shapes VALUES (r.name, r.class, coldiff.header(s) ->> 'body');
		n := n + 1;
	END LOOP;
	RETURN n;
END $$;

-- the cost and the shape of the plan of each way, for every shape (EXPLAIN only)
CREATE FUNCTION plangate.cal_estimate() RETURNS void LANGUAGE plpgsql AS $$
DECLARE s record; v text; j jsonb; c float8; t text; l text;
BEGIN
	FOR s IN SELECT * FROM plangate.cal_shapes ORDER BY name LOOP
		FOREACH v IN ARRAY ARRAY['scan', 'seq', 'index', 'bitmap', 'native', 'chosen'] LOOP
			PERFORM plangate.cal_set(v);
			EXECUTE 'EXPLAIN (FORMAT JSON) ' || s.body INTO j;
			c := (j -> 0 -> 'Plan' ->> 'Total Cost')::float8;
			t := '';
			FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || s.body LOOP t := t || l || E'\n'; END LOOP;
			-- a node a setting disabled costs 1e10 more before PostgreSQL 18 (taken off, as plangate.cost does) and PostgreSQL 18 says Disabled: true, which is not another plan
			t := regexp_replace(t, E'\\n *Disabled: true', '', 'g');
			INSERT INTO plangate.cal_plans VALUES (s.name, v, c - 1e10 * floor(c / 1e10), md5(t), j::text LIKE '%"SiftAgg"%');
		END LOOP;
	END LOOP;
	PERFORM plangate.cal_set('chosen');
END $$;

-- run (or only plan) a shape n times in this session, for the instructions the script counts
CREATE FUNCTION plangate.cal_run(nm text, n int, plan_only boolean) RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text; r record;
BEGIN
	SELECT body INTO q FROM plangate.cal_shapes WHERE name = nm;
	FOR i IN 1..n LOOP
		IF plan_only THEN
			FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP NULL; END LOOP;
		ELSE
			FOR r IN EXECUTE q LOOP NULL; END LOOP;
		END IF;
	END LOOP;
END $$;

-- the verdict of a shape: the plan the planner chooses against the best measured one (instructions, planning taken off)
CREATE VIEW plangate.cal_verdicts AS
	WITH p AS (
		SELECT name, sig, min(cost) AS cost, coalesce(string_agg(variant, '=' ORDER BY variant) FILTER (WHERE variant <> 'chosen'), 'chosen') AS variants,
			   bool_or(variant = 'chosen') AS chosen, bool_or(sift) AS sift
		FROM plangate.cal_plans GROUP BY name, sig),
	m AS (
		SELECT p.name, p.sig, p.cost, p.variants, p.chosen, p.sift, e.instr FROM p JOIN plangate.cal_measured e USING (name, sig)),
	b AS (SELECT name, min(instr) AS best FROM m GROUP BY name)
	SELECT s.class, m.name, m.variants, m.chosen, m.sift, round(m.cost::numeric, 1) AS cost, round((m.instr / 1e6)::numeric, 2) AS m_instr,
		   rank() OVER (PARTITION BY m.name ORDER BY m.cost) AS rank_est, rank() OVER (PARTITION BY m.name ORDER BY m.instr) AS rank_meas,
		   round((b.best / 1e6)::numeric, 2) AS best_m_instr,
		   m.chosen AND m.instr > 1.2 * b.best AND m.instr > b.best + 1e6 AS wrong
	FROM m JOIN b USING (name) JOIN plangate.cal_shapes s USING (name);

-- the pairs of plans of a shape the estimates rank the wrong way round: the dearer by estimate takes fewer instructions, by more than 20% and 1,000,000. The plan the
-- planner chooses is not in a pair: it is the ranking (the planner prices the plan without the scan higher than sift off does, so the cost of the plan sift off
-- makes is not what the choice meets), and the check on it is `wrong`.
-- sift: one of the two is a Custom Scan (SiftAgg), whose estimate is ours; the others are PostgreSQL's own (and the tuned lane's seq_page_cost prices pages, which instructions do not count)
CREATE VIEW plangate.cal_pairs AS
	SELECT a.class, a.name, a.variants AS estimated_cheaper, b.variants AS estimated_dearer, a.cost AS cost_a, b.cost AS cost_b, a.m_instr AS m_a, b.m_instr AS m_b, a.sift OR b.sift AS sift
	FROM plangate.cal_verdicts a JOIN plangate.cal_verdicts b ON a.name = b.name AND NOT a.chosen AND NOT b.chosen AND a.cost < b.cost AND a.m_instr > 1.2 * b.m_instr AND a.m_instr > b.m_instr + 1;
