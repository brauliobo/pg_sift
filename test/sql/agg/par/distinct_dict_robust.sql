-- COUNT(DISTINCT) of a text column read as dictionary codes (src/agg/dictseen.c) where the plan does not run to its end or runs again: a rescan of the node under a nested loop, a LIMIT, a statement
-- cancelled in the middle of the scan, of the export of the sets and of the merge, a node with no worker launched. After every one the session answers with the heap's own aggregate, and no temporary
-- file or backend of the cancelled statement is left.
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
SELECT i, (i * 7919) % 13 AS k, CASE WHEN i % 23 = 0 THEN NULL ELSE 'w' || (i * 7 % 311) END AS tc,
       CASE WHEN i % 17 = 0 THEN NULL WHEN i <= 20000 THEN 'm' || (i * 13 % 57) ELSE 'u' || md5((i % 6001)::text) END AS tm
FROM generate_series(1::int8, 60000::int8) AS i;
CREATE INDEX pr_col ON pr USING sift_col (k, tc, tm);
VACUUM (ANALYZE, FREEZE) pr;
CREATE FUNCTION plan_of(q text) RETURNS jsonb LANGUAGE plpgsql AS $f$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	RETURN j;
END $f$;
SELECT plan::text LIKE '%"Node Type": "Gather"%' AS gathers, substring(plan::text from '"Parallel Strategy": "([^"]*)"') AS strategy
FROM plan_of('SELECT count(DISTINCT tc), count(DISTINCT tm) FROM pr') AS plan;
-- a LIMIT without an ORDER BY: the Gather stops reading and the node shuts down with its workers still producing
SELECT count(*) AS limited FROM (SELECT k, count(DISTINCT tc) FROM pr GROUP BY k LIMIT 3) AS q;
SELECT count(*) AS limited FROM (SELECT count(DISTINCT tm) FROM pr LIMIT 1) AS q;
-- a rescan: the node under a nested loop runs once for every row of the outer side, and every run is the whole answer
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT s.i, q.c, q.c = (SELECT count(DISTINCT tc) FROM pr) AS whole_answer
FROM generate_series(1, 3) AS s(i) JOIN LATERAL (SELECT count(DISTINCT tc) AS c FROM pr WHERE s.i > 0) AS q ON true ORDER BY s.i;
SELECT s.i, q.c, q.m FROM generate_series(1, 3) AS s(i) JOIN LATERAL (SELECT count(DISTINCT tc) AS c, count(DISTINCT tm) AS m FROM pr WHERE k < s.i * 4) AS q ON true ORDER BY s.i;
RESET enable_material;
RESET enable_hashjoin;
RESET enable_mergejoin;
SET sift.enable_agg = off;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ref_dist AS SELECT count(DISTINCT tc) AS dc, count(DISTINCT tm) AS dm FROM pr;
CREATE TABLE ref_grp AS SELECT k, count(DISTINCT tc) AS dc, count(DISTINCT tm) AS dm FROM pr GROUP BY k;
SELECT * FROM ref_dist;
SET sift.enable_agg = on;
SET max_parallel_workers_per_gather = 4;
-- a statement cancelled at a growing time: the next statement of the session runs to its end with the right answer
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
SELECT bool_and(cancelled_then_sound('SELECT count(DISTINCT tc) AS dc, count(DISTINCT tm) AS dm FROM pr', 'ref_dist', ms)) AS distinct_sound FROM unnest(ARRAY[1, 3, 10, 30, 100, 300]) AS ms;
SELECT bool_and(cancelled_then_sound('SELECT k, count(DISTINCT tc) AS dc, count(DISTINCT tm) AS dm FROM pr GROUP BY k', 'ref_grp', ms)) AS grouped_sound FROM unnest(ARRAY[1, 3, 10, 30, 100, 300]) AS ms;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT bool_and(cancelled_then_sound('SELECT count(DISTINCT tc) AS dc, count(DISTINCT tm) AS dm FROM pr', 'ref_dist', ms)) AS small_budget_sound FROM unnest(ARRAY[1, 3, 10, 30, 100, 300]) AS ms;
RESET work_mem;
RESET hash_mem_multiplier;
-- nothing of a cancelled statement stays: no temporary file, no leftover backend
SELECT count(*) AS temp_files_left FROM pg_ls_tmpdir();
SELECT count(*) AS parallel_workers_left FROM pg_stat_activity WHERE backend_type = 'parallel worker';
-- every worker slot taken: no worker is launched, the leader runs the node alone
SET max_parallel_workers = 0;
CREATE TABLE p0_dist AS SELECT count(DISTINCT tc) AS dc, count(DISTINCT tm) AS dm FROM pr;
SELECT (SELECT count(*) FROM (SELECT * FROM ref_dist EXCEPT ALL SELECT * FROM p0_dist) a) + (SELECT count(*) FROM (SELECT * FROM p0_dist EXCEPT ALL SELECT * FROM ref_dist) b) AS no_worker_diff;
DROP TABLE p0_dist, pr, ref_dist, ref_grp;
DROP FUNCTION cancelled_then_sound(text,regclass,integer), plan_of(text);
DROP EXTENSION pg_sift;
