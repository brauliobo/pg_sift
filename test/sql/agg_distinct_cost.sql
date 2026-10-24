-- count(DISTINCT) of a column keeps a bitmap per group, in chunks of 4096 values that a group allocates and zeroes when one of its values first falls in
-- (about 0.7 microseconds a chunk), so a query with thousands of groups pays for them while one with a hundred does not. The scan is priced for the chunks
-- while they fit the memory of the set (work_mem times hash_mem_multiplier): past it the set moves to a hash set whose pair costs between 0.1 and 2
-- microseconds with the column and the size of the set, which is not priced. The statements run with enable_agg on and off: the rows must be the same.
-- The workers of a scan are priced for their start (3 ms and 0.35 ms each) and, for a count(DISTINCT), for the leader merging the pairs of the sets one by one;
-- a parallel_setup_cost of 0 asks for them.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 2;
CREATE TABLE items (id int PRIMARY KEY, grp int NOT NULL, part int NOT NULL, cat int NOT NULL, amount float8 NOT NULL);
SELECT setseed(0.42);
INSERT INTO items SELECT g, (random() * 4999)::int, g % 100, (random() * 999)::int, random() * 100 FROM generate_series(1, 100000) g;
CREATE INDEX items_grp ON items (grp);
CREATE INDEX items_col ON items USING sift_col (id, grp, part, cat, amount);
VACUUM (FREEZE, ANALYZE) items;

CREATE FUNCTION scan_cost(q text) RETURNS numeric LANGUAGE sql AS $$ SELECT scan_cost::numeric FROM sift.explain_why(q) WHERE scan_cost IS NOT NULL $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- 5,000 groups of a column of 1,000 values (one chunk a group) cost the scan well over the same groups of a column of 100,000 values (25 chunks a group, 35 MB:
-- more than the set holds, so they are not priced); with 100 groups the two cost the same
SELECT groups,
       scan_cost(format('SELECT %s, count(*) AS n, count(DISTINCT cat) AS d FROM items GROUP BY %s', g, g)) / scan_cost(format('SELECT %s, count(*) AS n, count(DISTINCT id) AS d FROM items GROUP BY %s', g, g)) > 1.5 AS chunks_priced,
       differences(format('SELECT %s, count(*) AS n, count(DISTINCT cat) AS d FROM items GROUP BY %s', g, g)) AS differences
FROM (VALUES ('5000 groups', 'grp'), ('100 groups', 'part')) v(groups, g) ORDER BY groups;

-- a column of 100 values (a window of two words) keeps its sets in the words of the group (agg/dist.c): no chunk to price, for any number of groups
SELECT scan_cost('SELECT grp, count(*) AS n, count(DISTINCT cat) AS d FROM items GROUP BY grp') / scan_cost('SELECT grp, count(*) AS n, count(DISTINCT part) AS d FROM items GROUP BY grp') > 1.5 AS words_not_priced,
       differences('SELECT grp, count(*) AS n, count(DISTINCT part) AS d FROM items GROUP BY grp') AS differences;

-- workers planned for the scan of a table priced as 100 times larger (sift.plan_scale): a sum scales with them, a count(DISTINCT) does not (the leader merges
-- every pair), and a start that costs 3 ms keeps a scan of a few milliseconds serial whatever the setup cost; 0 asks for the workers
CREATE FUNCTION workers(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN coalesce((SELECT max(m[1]::int) FROM regexp_matches(j::text, '"Workers Planned": (\d+)', 'g') m), 0);
END $$;
SET min_parallel_table_scan_size = 0;
SET parallel_tuple_cost = 0;
SET sift.agg_preagg = off;
SET sift.plan_scale = 100;
CREATE FUNCTION workers_at(setup int) RETURNS TABLE (setup_cost int, sum_workers int, grouped_distinct_workers int, global_distinct_workers int) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('parallel_setup_cost', setup::text, true);
	RETURN QUERY SELECT setup, workers('SELECT part, sum(amount) FROM items GROUP BY part'), workers('SELECT grp, count(DISTINCT cat) FROM items GROUP BY grp'),
		workers('SELECT count(DISTINCT cat) FROM items');
END $$;
SELECT * FROM workers_at(0) UNION ALL SELECT * FROM workers_at(1) UNION ALL SELECT * FROM workers_at(100) ORDER BY 1;
RESET sift.plan_scale;
SELECT sum_workers AS small_scan_workers FROM workers_at(1);
RESET parallel_setup_cost; RESET min_parallel_table_scan_size; RESET parallel_tuple_cost; RESET sift.agg_preagg;

DROP FUNCTION scan_cost(text), differences(text), workers(text), workers_at(int);
DROP TABLE items;
DROP EXTENSION pg_sift;
