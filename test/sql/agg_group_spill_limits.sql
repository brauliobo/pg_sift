-- The limits of the group table that spills (docs/LIMITS.md), lowered with sift.test_limits so that a few thousand rows reach them:
-- the levels of partitions, the partitions of a level, and the size of a table that cannot spill.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '60s';
SET client_min_messages = warning;

CREATE TABLE events (id int, k1 int, val int, w float8);
INSERT INTO events SELECT i, (i * 7919) % 20000, i % 1000, (i % 977) * 1.25 FROM generate_series(1, 40000) i;
CREATE INDEX events_col ON events USING sift_col (id, k1, val, w);
VACUUM (FREEZE, ANALYZE) events;

SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET enable_hashagg = off;
SET enable_sort = off;

-- the groups of the query, as the normal plan returns them, to compare with the scan that spills
SET sift.enable_agg = off;
CREATE TEMP TABLE want AS SELECT k1, count(*) AS n, sum(val) AS s, sum(w) AS t FROM events GROUP BY k1;
RESET sift.enable_agg;

CREATE FUNCTION spilled_rows(q text) RETURNS TABLE (spills int, rows_same bigint) LANGUAGE plpgsql AS $$
DECLARE l text; sp int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Group Table Spills: ' THEN sp := regexp_replace(l, '^.*Spills: ', '')::int; END IF;
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE got AS ' || q;
	RETURN QUERY SELECT sp, (SELECT count(*) FROM (TABLE want EXCEPT TABLE got) d);
	DROP TABLE got;
END $$;

-- the partitions of a level: 8 at least, as many as the limit allows
SET sift.test_limits = 'group_spill_parts=8';
SELECT * FROM spilled_rows('SELECT k1, count(*) AS n, sum(val) AS s, sum(w) AS t FROM events GROUP BY k1');
RESET sift.test_limits;

-- a partition that does not fit hash memory after the levels the limit allows is an error that says what to raise
SET sift.test_limits = 'group_spill_depth=1';
DO $$
DECLARE hint text;
BEGIN
	PERFORM count(*) FROM (SELECT k1, count(*) AS n, sum(val) AS s, sum(w) AS t FROM events GROUP BY k1) q;
	RAISE NOTICE 'no error';
EXCEPTION WHEN program_limit_exceeded THEN
	GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT;
	RAISE WARNING 'SQLSTATE % hint: %', SQLSTATE, hint;
END $$;
RESET sift.test_limits;

-- a table that cannot spill: count(DISTINCT) of 20,000 groups is 4 MB, under the 8 MB the factor applies to; with a factor of 0 the scan gives way
RESET work_mem;
RESET hash_mem_multiplier;
SELECT arm, used, slug FROM sift.explain_why('SELECT k1, count(DISTINCT val) FROM events GROUP BY k1');
SET sift.test_limits = 'group_nospill_factor=0';
SELECT arm, used, slug, reason ~ 'cannot spill them past 0 times' AS names_factor FROM sift.explain_why('SELECT k1, count(DISTINCT val) FROM events GROUP BY k1');
RESET sift.test_limits;

DROP FUNCTION spilled_rows(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
