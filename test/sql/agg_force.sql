-- sift.agg_force takes the aggregate scan wherever the planner can build it, whatever the plans cost, to time the plan the cost model refused: a probe
-- of one key keeps its index scan by cost, and a shared CTE of a few rows that two scalar subqueries read stays shared; forced, both take the scan. A statement the scan
-- cannot compute stays refused for its shape. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, kind text NOT NULL, amount float8) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, i % 5000, 'k' || i % 5, i % 17 FROM generate_series(1, 60000) i;
CREATE INDEX events_parent ON events (parent_id);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, amount);
VACUUM (FREEZE, ANALYZE) events;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN CASE WHEN j::text LIKE '%"SiftAgg"%' THEN 'SiftAgg' ELSE 'normal' END;
END $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	DROP TABLE r_on, r_off;
	RETURN a = 0;
END $$;

CREATE FUNCTION probe() RETURNS text LANGUAGE sql AS $$ SELECT 'SELECT kind, count(*) AS n, sum(amount) AS s FROM events WHERE parent_id = 7 GROUP BY 1' $$;
CREATE FUNCTION shared() RETURNS text LANGUAGE sql AS $$ SELECT 'WITH c AS (SELECT kind, parent_id FROM events WHERE amount = 3 AND id < 5000) SELECT (SELECT sum(parent_id) FROM c), (SELECT count(*) FROM c)' $$;

-- by cost: the index scan of the probe
SELECT plan_of(probe()) AS probe, (SELECT slug FROM sift.explain_why(probe()) WHERE NOT used LIMIT 1) AS slug;
SELECT used, slug FROM sift.explain_why(shared()) WHERE arm = 'cte_inline';
SET sift.agg_force = on;
SELECT plan_of(probe()) AS probe_forced, (SELECT bool_and(used) FROM sift.explain_why(probe()) WHERE arm <> 'cte_inline') AS used;
SELECT same(probe()) AS probe_same;
SELECT used, slug FROM sift.explain_why(shared()) WHERE arm = 'cte_inline';
-- a shape the scan does not compute stays refused
SELECT plan_of('SELECT count(*) FROM events e WHERE e.parent_id IN (SELECT id FROM events WHERE amount > 5 LIMIT 3) AND random() < 2') AS volatile_forced;
SELECT same(shared()) AS shared_same;
RESET sift.agg_force;
SELECT plan_of(probe()) AS probe_again;

-- the forced scan keeps the order of the prices of its paths: where the scan with workers is the cheaper of the two, it gets them
CREATE FUNCTION workers_of(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		n := n + coalesce(substring(l from 'Workers Planned: (\d+)')::int, 0);
	END LOOP;
	RETURN n;
END $$;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET sift.agg_force = on;
SELECT workers_of('SELECT kind, count(*) AS n, sum(amount) AS s FROM events GROUP BY 1') AS forced_workers;
SET sift.agg_force = off;
SELECT workers_of('SELECT kind, count(*) AS n, sum(amount) AS s FROM events GROUP BY 1') > 0 AS natural_workers;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SELECT workers_of('SELECT kind, count(*) AS n, sum(amount) AS s FROM events GROUP BY 1') AS forced_serial;
RESET sift.agg_force;

DROP FUNCTION plan_of(text), same(text), probe(), shared(), workers_of(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
