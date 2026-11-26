-- Over a sift_col index of Format 2.0 the CTE inlining and the flat rewrites of the planner hook engage as they do over Format 1, and SiftAggV6 plans the rewritten statement: a CTE read once is
-- inlined into its one scan, a CTE read twice stays shared over the scan, and a correlated scalar sub-select becomes a join of scans, without a SubPlan.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE orders (id int PRIMARY KEY, source text NOT NULL, region int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO orders SELECT g, (ARRAY['web', 'app', 'store', 'phone'])[1 + g % 4], g % 7, g % 1000 FROM generate_series(1, 50000) g;
CREATE INDEX orders_col ON orders USING sift_col (id, source, region, amount);
VACUUM (FREEZE, ANALYZE) orders;

CREATE FUNCTION plan_shape(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	RETURN concat_ws(' ', CASE WHEN p ~ 'SiftAggV6' THEN 'v6' END, CASE WHEN p ~ 'CTE Scan' THEN 'cte-scan' END, CASE WHEN p ~ 'SubPlan' THEN 'subplan' END, CASE WHEN p ~ 'Seq Scan' THEN 'seqscan' END);
END $$;

SELECT q.name, plan_shape(q.sql) AS shape
FROM (VALUES ('cte_once',  $q$WITH c AS (SELECT source, count(*) n FROM orders GROUP BY 1) SELECT * FROM c WHERE n > 0$q$),
             ('cte_twice', $q$WITH c AS (SELECT source, region, count(*) n FROM orders GROUP BY 1, 2) SELECT a.source, sum(a.n) FROM c a JOIN c b USING (source, region) GROUP BY 1$q$),
             ('correlated', $q$SELECT a.region, (SELECT max(amount) FROM orders b WHERE b.region = a.region) FROM (SELECT region FROM orders GROUP BY region) a$q$)) q(name, sql)
ORDER BY 1;

WITH c AS (SELECT source, count(*) n FROM orders GROUP BY 1) SELECT source, n FROM c ORDER BY 1;
SELECT a.region, (SELECT max(amount) FROM orders b WHERE b.region = a.region) FROM (SELECT region FROM orders GROUP BY region) a ORDER BY 1;

-- the correlated sub-select in the format of the run (Format 1.0 too), over 300 rows: a join of scans, with a NULL key, and the rows of the normal plan
RESET sift.test_create_format1;
CREATE TABLE small (id int PRIMARY KEY, region int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO small SELECT g, CASE WHEN g % 31 = 0 THEN NULL ELSE g % 7 END, g % 100 FROM generate_series(1, 300) g;
CREATE INDEX small_col ON small USING sift_col (id, region, amount);
VACUUM (FREEZE, ANALYZE) small;
\set q 'SELECT a.region, (SELECT min(amount) FROM small b WHERE b.region = a.region AND b.amount > 10) AS lo, (SELECT sum(amount) FROM small b WHERE b.region = a.region) AS s FROM (SELECT region FROM small GROUP BY region) a'
SET sift.agg_force = on;
SELECT plan_shape(:'q') AS shape;
CREATE TEMP TABLE r_on AS :q;
SET sift.enable_agg = off;
CREATE TEMP TABLE r_off AS :q;
RESET sift.enable_agg;
RESET sift.agg_force;
SELECT (SELECT count(*) FROM r_off) AS n, (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x) AS differences;
DROP TABLE r_on, r_off, small;

DROP FUNCTION plan_shape(text);
DROP TABLE orders;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
