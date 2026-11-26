-- The parallel SiftAggV6 (Spec 44) where a plan does not run to its end or runs again: a LIMIT that stops the Gather early, a rescan of the node under a nested loop, a statement cancelled in the
-- middle of each phase of the radix and the two-stage strategies, a query with no worker launched and one with every worker busy. After every one the server answers, and no temporary file or
-- segment of the cancelled statement is left. Needs the extension; the answers are held to the heap's own aggregate (sift.enable_agg = off).
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET sift.test_agg_par_min_rows = 0;
SET max_parallel_workers_per_gather = 4;
CREATE TABLE pr AS
SELECT i, ((i * 7919) % 21001)::int8 AS g, (i * 31 % 1000)::int8 AS v, (((i * 104729) % 100000)::numeric / 100)::numeric(12,2) AS n, 'k' || (i * 7 % 311) AS t, (i * 40503) % 4801 AS d
FROM generate_series(1::int8, 80000::int8) AS i;
CREATE INDEX pr_col ON pr USING sift_col (g, v, n, t, d);
VACUUM (ANALYZE, FREEZE) pr;
CREATE FUNCTION plan_of(q text) RETURNS jsonb LANGUAGE plpgsql AS $f$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	RETURN j;
END $f$;
-- the queries below take the parallel strategies named here
SELECT strategy, plan::text LIKE '%"Node Type": "Gather"%' AS gathers, substring(plan::text from '"Parallel Strategy": "([^"]*)"') = strategy AS named
FROM (VALUES ('radix', 'SELECT g, count(*), sum(n) FROM pr GROUP BY g'), ('radix distinct', 'SELECT count(DISTINCT d), count(DISTINCT t) FROM pr'),
             ('two-stage', 'SELECT t, count(*), sum(n), min(v), max(v) FROM pr GROUP BY t')) AS v(strategy, q), LATERAL plan_of(q) AS plan;
-- a LIMIT without an ORDER BY: the Gather stops reading and the node shuts down with its workers still producing
SELECT count(*) AS limited FROM (SELECT g, count(*) FROM pr GROUP BY g LIMIT 10) AS q;
SELECT count(*) AS limited FROM (SELECT t, count(*) FROM pr GROUP BY t LIMIT 5) AS q;
SELECT count(*) AS limited FROM (SELECT g, count(DISTINCT d) FROM pr GROUP BY g LIMIT 7) AS q;
-- a rescan: the node under a nested loop runs once for every row of the outer side, and every run is the whole answer
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT plan::text LIKE '%"Node Type": "Gather"%' AS rescan_plan_gathers, plan::text LIKE '%"Node Type": "Nested Loop"%' AS nested_loop
FROM plan_of('SELECT s.i, q.c FROM generate_series(1, 3) AS s(i) JOIN LATERAL (SELECT count(*) AS c FROM (SELECT g, count(*) FROM pr GROUP BY g) AS x WHERE s.i > 0) AS q ON true') AS plan;
SELECT s.i, q.c, q.c = (SELECT count(DISTINCT g) FROM pr) AS whole_answer
FROM generate_series(1, 3) AS s(i) JOIN LATERAL (SELECT count(*) AS c FROM (SELECT g, count(*) FROM pr GROUP BY g) AS x WHERE s.i > 0) AS q ON true ORDER BY s.i;
SELECT s.i, q.c FROM generate_series(1, 3) AS s(i) JOIN LATERAL (SELECT count(DISTINCT d) AS c FROM pr WHERE s.i > 0) AS q ON true ORDER BY s.i;
RESET enable_material;
RESET enable_hashjoin;
RESET enable_mergejoin;
-- the reference of what a statement answers when it runs to its end
SET sift.enable_agg = off;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ref_radix AS SELECT g, count(*) AS c, sum(n) AS s FROM pr GROUP BY g;
CREATE TABLE ref_dist AS SELECT count(DISTINCT d) AS dd, count(DISTINCT t) AS dt FROM pr;
CREATE TABLE ref_two AS SELECT t, count(*) AS c, sum(n) AS s, min(v) AS mn, max(v) AS mx FROM pr GROUP BY t;
SET sift.enable_agg = on;
SET max_parallel_workers_per_gather = 4;
-- a statement cancelled at a growing time: the error is the cancel, and the next statement of the session runs to its end with the right answer (the number of statements that were cancelled is not printed: it depends on the machine)
CREATE FUNCTION cancelled_then_sound(q text, ref regclass, ms int) RETURNS boolean LANGUAGE plpgsql AS $f$
DECLARE n bigint;
BEGIN
	PERFORM set_config('statement_timeout', ms::text, true);
	BEGIN
		EXECUTE q;
	EXCEPTION WHEN query_canceled THEN
		NULL;
	END;
	PERFORM set_config('statement_timeout', '0', true);
	EXECUTE format('SELECT count(*) FROM (SELECT r FROM (SELECT x::text AS r, 1 AS w FROM %1$s x UNION ALL SELECT y::text, -1 FROM (%2$s) y) u GROUP BY r HAVING sum(w) <> 0) d', ref, q) INTO n;
	RETURN n = 0;
END $f$;
SELECT bool_and(cancelled_then_sound('SELECT g, count(*) AS c, sum(n) AS s FROM pr GROUP BY g', 'ref_radix', ms)) AS radix_sound FROM unnest(ARRAY[1, 3, 10, 30, 100, 300]) AS ms;
SELECT bool_and(cancelled_then_sound('SELECT count(DISTINCT d) AS dd, count(DISTINCT t) AS dt FROM pr', 'ref_dist', ms)) AS distinct_sound FROM unnest(ARRAY[1, 3, 10, 30, 100, 300]) AS ms;
SELECT bool_and(cancelled_then_sound('SELECT t, count(*) AS c, sum(n) AS s, min(v) AS mn, max(v) AS mx FROM pr GROUP BY t', 'ref_two', ms)) AS two_stage_sound FROM unnest(ARRAY[1, 3, 10, 30, 100, 300]) AS ms;
-- nothing of a cancelled statement stays: no temporary file, no leftover backend
SELECT count(*) AS temp_files_left FROM pg_ls_tmpdir();
SELECT count(*) AS parallel_workers_left FROM pg_stat_activity WHERE backend_type = 'parallel worker';
-- every worker slot taken by other sessions' parallel queries: no worker is launched, the leader runs the node alone
SET max_parallel_workers = 0;
CREATE TABLE p0_radix AS SELECT g, count(*) AS c, sum(n) AS s FROM pr GROUP BY g;
CREATE TABLE p0_two AS SELECT t, count(*) AS c, sum(n) AS s, min(v) AS mn, max(v) AS mx FROM pr GROUP BY t;
SELECT (SELECT count(*) FROM (SELECT * FROM ref_radix EXCEPT ALL SELECT * FROM p0_radix) a) + (SELECT count(*) FROM (SELECT * FROM p0_radix EXCEPT ALL SELECT * FROM ref_radix) b) AS radix_no_worker_diff,
       (SELECT count(*) FROM (SELECT * FROM ref_two EXCEPT ALL SELECT * FROM p0_two) a) + (SELECT count(*) FROM (SELECT * FROM p0_two EXCEPT ALL SELECT * FROM ref_two) b) AS two_stage_no_worker_diff;
DROP TABLE p0_radix, p0_two, pr, ref_dist, ref_radix, ref_two;
DROP FUNCTION cancelled_then_sound(text,regclass,integer), plan_of(text);
DROP EXTENSION pg_sift;
