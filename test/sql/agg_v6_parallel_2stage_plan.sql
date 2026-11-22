-- The plans of the parallel SiftAggV6 (Spec 44): a Gather over a SiftAggV6 that names its strategy, the strategy the shape of each query asks for, and the settings that keep a query serial.
-- Needs the extension (the first query is the proof that the sift path is taken); expected output is not generated here: run it in the integration tree, REGEN there.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
CREATE TABLE pe AS
SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE (i * 7919) % 50021 END AS g, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 31) % 1000 END AS v,
       (CASE WHEN i % 11 = 0 THEN NULL ELSE ((i * 104729) % 100000)::numeric / 8 END)::numeric(12,3) AS n, (i * 40503) % 977 AS d,
       CASE WHEN i % 19 = 0 THEN NULL ELSE 'k' || (i * 7 % 311) END AS t, ((i * 40503) % 1009)::float8 / 16 AS f
FROM generate_series(1::int8, 60000::int8) AS i;
CREATE INDEX pe_col ON pe USING sift_col (g, v, n, d, t, f);
ANALYZE pe;
CREATE FUNCTION plan_of(q text, settings text DEFAULT '') RETURNS jsonb LANGUAGE plpgsql AS $f$
DECLARE j jsonb;
BEGIN
	EXECUTE settings;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	RETURN j;
END $f$;
CREATE FUNCTION undo(n text, v text) RETURNS boolean LANGUAGE plpgsql AS $f$ BEGIN PERFORM set_config(n, v, false); RETURN true; END $f$;
-- every shape of agg_v6_parallel_2stage.sql: a Gather above the node, the strategy the node names, and that the workers stole morsels
SELECT shape, plan::text LIKE '%"Node Type": "Gather"%' AS gathers, plan::text LIKE '%"Custom Plan Provider": "SiftAggV6"%' AS sift_node,
       substring(plan::text from '"Parallel Strategy": "([^"]*)"') AS strategy, plan::text LIKE '%"Morsels Stolen"%' AS morsels
FROM (VALUES ('q4', 'SELECT g, count(*) AS c, sum(v) AS s, min(v) AS mn, max(n) AS mx, avg(n) AS a FROM pe GROUP BY g'),
             ('q5', 'SELECT count(DISTINCT d) AS dd, count(DISTINCT t) AS dt, count(DISTINCT g) AS dg FROM pe'),
             ('low', 'SELECT t, count(*) AS c, sum(n) AS s, min(d) AS mn, max(d) AS mx, avg(v) AS a FROM pe GROUP BY t'),
             ('grouped_distinct', 'SELECT t, count(DISTINCT g) AS dg, count(DISTINCT d) AS dd, count(*) AS c FROM pe GROUP BY t'),
             ('ungrouped', 'SELECT count(*) AS c, count(v) AS cv, sum(n) AS s, avg(v) AS a, min(n) AS mn, max(n) AS mx FROM pe'),
             ('sets', 'SELECT t, v, GROUPING(t, v) AS gr, count(*) AS c, sum(d) AS s FROM pe GROUP BY GROUPING SETS ((t), (v), (t, v), ())'),
             ('having', 'SELECT g, count(*) AS c FROM pe GROUP BY g HAVING count(*) > 1'),
             ('filter', 'SELECT t, count(*) FILTER (WHERE d < 100) AS lo, sum(v) FILTER (WHERE d >= 100) AS hi FROM pe GROUP BY t'),
             ('float', 'SELECT t, sum(f) AS s, avg(f) AS a FROM pe GROUP BY t')) AS v(shape, q), LATERAL plan_of(q) AS plan;
-- what keeps a query serial: the session, a float sum under exact_float, no worker allowed; the baseline shows the same query gathers
SELECT s AS setting, plan_of('SELECT t, sum(f) FROM pe GROUP BY t', format('SET %s = %s', n, v))::text LIKE '%"Node Type": "Gather"%' AS gathers, undo(n, u)
FROM (VALUES ('the baseline', 'max_parallel_workers_per_gather', '4', '4'), ('sift.parallel_scan = off', 'sift.parallel_scan', 'off', 'on'),
             ('max_parallel_workers_per_gather = 0', 'max_parallel_workers_per_gather', '0', '4'), ('sift.agg_exact_float = on', 'sift.agg_exact_float', 'on', 'off')) AS v(s, n, v, u);
DROP FUNCTION undo(text, text);
DROP FUNCTION plan_of(text, text);
DROP TABLE pe;
DROP EXTENSION pg_sift;
