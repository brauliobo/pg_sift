-- An aggregate whose groups the planner underestimates by more than group_nospill_factor (statistics that predate a bulk load) returns the rows the
-- normal plan returns. Where the scan can spill, the groups of its table go to partitions on disk (grouping sets, with the sets folded into the coarser ones
-- before each table is written); where it cannot (an ordered scan on a text key, a generic aggregate) the statement is run again as PostgreSQL plans it.
-- The bound on a table that cannot spill is lowered so that a few thousand groups pass it.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

CREATE TABLE stale (id int, g int, v int);
ALTER TABLE stale SET (autovacuum_enabled = false);
INSERT INTO stale SELECT i, i % 10, i % 100 FROM generate_series(1, 400) i;
CREATE INDEX stale_col ON stale USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) stale;
-- 20,000 groups where the statistics say 10
INSERT INTO stale SELECT i, i, i % 100 FROM generate_series(401, 20400) i;

SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET sift.agg_force = on;
SET sift.test_limits = 'group_nospill_factor=1,group_nospill_base=65536';

-- one statement: the plan it gets, whether its table spilled, and the rows of the scan against the rows of the normal plan
CREATE FUNCTION stale_case(q text) RETURNS TABLE (shape text, spilled boolean, rows bigint, differ bigint) LANGUAGE plpgsql AS $$
DECLARE l text; scan boolean := false; sp boolean := false;
BEGIN
	DROP TABLE IF EXISTS got;
	DROP TABLE IF EXISTS want;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		scan := scan OR l ~ 'SiftAgg';
		sp := sp OR l ~ '[1-9][0-9]* group table spills';
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE got AS ' || q;
	SET sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE want AS ' || q;
	RESET sift.enable_agg;
	RETURN QUERY SELECT CASE WHEN scan THEN 'SiftAgg' ELSE 'heap' END, sp, (SELECT count(*) FROM got),
		(SELECT count(*) FROM (SELECT * FROM got EXCEPT ALL SELECT * FROM want UNION ALL SELECT * FROM want EXCEPT ALL SELECT * FROM got) d);
END $$;

-- grouping sets spill: the groups of every set, the total included, are the heap's
SELECT * FROM stale_case('SELECT g, count(*) AS n, sum(v) AS s FROM stale GROUP BY GROUPING SETS ((g), ())');
SELECT * FROM stale_case('SELECT g, v, count(*) AS n, min(id) AS m, avg(v) AS a FROM stale GROUP BY GROUPING SETS ((g, v), (g), ())');
SELECT * FROM stale_case('SELECT g, v, count(*) AS n, sum(v::float8) AS s FROM stale GROUP BY GROUPING SETS ((g), (v), ())');
SELECT * FROM stale_case('SELECT g, count(*) AS n, grouping(g) AS gr FROM stale GROUP BY GROUPING SETS ((g), ()) ORDER BY g');
SELECT * FROM stale_case('SELECT g, count(*) AS n FROM stale WHERE id < 0 GROUP BY GROUPING SETS ((g), ())');

-- the statements that cannot spill run again as PostgreSQL plans them
SET client_min_messages = notice;
SELECT * FROM stale_case('SELECT g::text AS g, count(*) AS n FROM stale GROUP BY g::text ORDER BY 1');
SELECT * FROM stale_case('SELECT g, bit_or(v) AS b FROM stale GROUP BY g');
SET client_min_messages = warning;

-- the rows reach the client once: the first run sent none (extended protocol, a parameter, a limit)
SET client_min_messages = notice;
SELECT g::text AS g, count(*) AS n FROM stale WHERE v >= $1 GROUP BY g::text ORDER BY 1 LIMIT 3 \bind 0 \g
SELECT g::text AS g, count(*) AS n FROM stale GROUP BY g::text ORDER BY 1 LIMIT 3;
SET client_min_messages = warning;

-- a prepared statement: the generic plan is the one that meets the table, and the plan cache starts over after the run
SET plan_cache_mode = force_generic_plan;
PREPARE q AS SELECT g::text AS g, count(*) AS n FROM stale GROUP BY g::text ORDER BY 1;
PREPARE s AS SELECT g, count(*) AS n, sum(v) AS s FROM stale GROUP BY GROUPING SETS ((g), ());
CREATE TEMP TABLE prep_got AS EXECUTE q;
CREATE TEMP TABLE prep_again AS EXECUTE q;
CREATE TEMP TABLE prep_gotsets AS EXECUTE s;
SET sift.enable_agg = off;
CREATE TEMP TABLE prep_want AS SELECT g::text AS g, count(*) AS n FROM stale GROUP BY g::text;
CREATE TEMP TABLE prep_wantsets AS SELECT g, count(*) AS n, sum(v) AS s FROM stale GROUP BY GROUPING SETS ((g), ());
RESET sift.enable_agg;
SELECT (SELECT count(*) FROM prep_got) AS rows,
       (SELECT count(*) FROM (TABLE prep_got EXCEPT ALL TABLE prep_want UNION ALL TABLE prep_want EXCEPT ALL TABLE prep_got) d) AS differ,
       (SELECT count(*) FROM (TABLE prep_again EXCEPT ALL TABLE prep_want UNION ALL TABLE prep_want EXCEPT ALL TABLE prep_again) d) AS differ_again,
       (SELECT count(*) FROM (TABLE prep_gotsets EXCEPT ALL TABLE prep_wantsets UNION ALL TABLE prep_wantsets EXCEPT ALL TABLE prep_gotsets) d) AS differ_sets;
DEALLOCATE q;
DEALLOCATE s;
RESET plan_cache_mode;

-- a cursor that fetches some rows cannot run again: the error names what to raise
DO $$
DECLARE msg text; hint text;
BEGIN
	EXECUTE 'DECLARE c CURSOR FOR SELECT g::text AS g, count(*) AS n FROM stale GROUP BY g::text ORDER BY 1';
	EXECUTE 'FETCH 10 FROM c';
	RAISE NOTICE 'no error';
EXCEPTION WHEN program_limit_exceeded THEN
	GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, hint = PG_EXCEPTION_HINT;
	RAISE WARNING 'SQLSTATE % names the table: % hint: %', SQLSTATE, msg ~ 'group table of the aggregate holds [0-9]+ kB and cannot spill', hint;
END $$;

DROP FUNCTION stale_case(text);
DROP TABLE stale;
DROP EXTENSION pg_sift;
