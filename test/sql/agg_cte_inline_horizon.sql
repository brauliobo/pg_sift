-- sift.agg_scale_horizon: where the scans of an inline shared CTE cost less than the work they replace but not by the margin, the statement is planned again at
-- the square root of the horizon times the rows and at the horizon times the rows (sift.plan_scale), and the inline plan is kept if it wins at both.
-- This statement is not that close call: the work of the shared plan is priced at 395 times the scans at the size of the table (the unnesting aggregates of
-- the three branches are charged; the scans really run 3.9 times faster), at 430 times at three times the rows and at 440 at ten, so the scans win at every
-- size and the setting changes nothing. The test keeps the guard that the plans at the larger sizes never take a clear win away, and that the row of the choice holds
-- the totals of both plans at each size.
-- No test reaches the horizon pair with a real close call: a shared CTE of plain aggregates (no unnesting) read twice puts the work of the shared plan at 0.6 to 7
-- times the scans, depending on how many aggregates read a document, and never between 1.15 and 1.6; the 156 and 237 statements of the load replay lie above 2.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
\set rows 130000
\ir test/agg_cte_inline_fixture.sql
CREATE FUNCTION inline_used(scale float8, horizon float8) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.plan_scale', scale::text, true), set_config('sift.agg_scale_horizon', horizon::text, true);
	RETURN coalesce((SELECT bool_or(used) FROM sift.explain_why(series_query()) WHERE arm = 'cte_inline'), false);
END $$;
SELECT scale, inline_used(scale, 1) AS horizon_off, inline_used(scale, 100) AS horizon_100 FROM unnest(ARRAY[1, 3, 10]) scale ORDER BY scale;
SELECT differences(series_query()) AS differences;

-- the row of the choice holds the totals of the two plans it compared, whichever it kept: the reason of a refusal names the same ones (a work_mem of its own
-- per call: a statement planned from a verdict cached reports no costs)
CREATE FUNCTION inline_costs(scale float8) RETURNS TABLE (has_costs boolean, same_as_reason boolean) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.plan_scale', scale::text, true), set_config('sift.agg_scale_horizon', '1', true), set_config('work_mem', (5000 + scale::int) || 'kB', true);
	RETURN QUERY SELECT w.scan_cost > 0 AND w.normal_cost > 0, w.used OR w.reason LIKE '%(inline costs ' || round(w.scan_cost::numeric, 2) || ' %the shared CTE ' || round(w.normal_cost::numeric, 2) || ' %'
		FROM sift.explain_why(series_query()) w WHERE w.arm = 'cte_inline';
END $$;
SELECT scale, (inline_costs(scale)).* FROM unnest(ARRAY[1, 3, 10]) scale ORDER BY scale;
DROP FUNCTION inline_costs;

DROP FUNCTION inline_used, differences, series_query;
DROP TABLE events, dims, maps;
DROP EXTENSION pg_sift;
