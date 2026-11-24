-- The parallel SiftAggV6 with ORDER BY .. LIMIT of the groups (every participant keeps its best K groups and only those cross the Gather) and with the aggregates whose states are not plain
-- accumulators (min and max of text, stddev, variance, bool_or, bool_and): the answers of a parallel run equal the heap's own aggregate, in the order the query asks.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 3;
CREATE TABLE pk AS
SELECT i, ((i * 7919) % 40009)::int8 AS g, (i * 31 % 1000)::int8 AS v, 's' || (i * 13 % 5003) AS t, repeat('x', (20 + i % 30)::int) || (i * 29 % 7001) AS u,
       ((i * 17) % 700)::float8 AS f, i % 3 = 0 AS b, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7 % 97)::int END AS k
FROM generate_series(1::int8, 400000::int8) AS i;
CREATE INDEX pk_col ON pk USING sift_col (g, v, t, u, f, b, k);
VACUUM (ANALYZE, FREEZE) pk;
CREATE FUNCTION plan_of(q text) RETURNS jsonb LANGUAGE plpgsql AS $f$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	RETURN j;
END $f$;
CREATE FUNCTION answer_of(q text) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE a text;
BEGIN
	EXECUTE 'SELECT string_agg(t::text, E''\n'') FROM (' || q || ') AS t' INTO a;
	RETURN a;
END $f$;
-- q runs in parallel (a Gather over the parallel node of the strategy), and answers what the heap answers; the rows of both are compared in the order of the statement
CREATE FUNCTION held(q text, OUT parallel boolean, OUT strategy text, OUT same boolean) LANGUAGE plpgsql AS $f$
DECLARE par text; ref text;
BEGIN
	parallel := plan_of(q)::text LIKE '%"Node Type": "Gather"%';
	strategy := substring(plan_of(q)::text from '"Parallel Strategy": "([^"]*)"');
	par := answer_of(q);
	SET LOCAL sift.enable_agg = off;
	SET LOCAL max_parallel_workers_per_gather = 0;
	ref := answer_of(q);
	same := par IS NOT DISTINCT FROM ref;
END $f$;
-- ORDER BY .. LIMIT of the groups: by an aggregate, by keys with NULLs, with an OFFSET, with a distinct count
SELECT * FROM held('SELECT g, count(*) AS c FROM pk GROUP BY g ORDER BY c DESC, g LIMIT 10');
SELECT * FROM held('SELECT g, count(*) AS c FROM pk GROUP BY g ORDER BY c DESC, g LIMIT 7 OFFSET 5');
SELECT * FROM held('SELECT k, g, count(*) AS c FROM pk GROUP BY k, g ORDER BY k NULLS FIRST, g DESC LIMIT 9');
SELECT * FROM held('SELECT k, g, sum(v) AS s FROM pk GROUP BY k, g ORDER BY s DESC NULLS LAST, k, g LIMIT 12');
SELECT * FROM held('SELECT g, count(DISTINCT v) AS d FROM pk GROUP BY g ORDER BY d DESC, g LIMIT 10');
SELECT * FROM held('SELECT t, g, count(*) AS c FROM pk GROUP BY t, g ORDER BY c DESC, t, g LIMIT 10');
-- a K larger than the groups of a participant, and than all of them
SELECT * FROM held('SELECT k, count(*) AS c FROM pk GROUP BY k ORDER BY c DESC, k NULLS LAST LIMIT 500');
-- min and max of text (the winners cross as bytes, the short ones inside the state), stddev, variance, bool_or and bool_and
SELECT * FROM held('SELECT g, min(u), max(u), count(*) AS c FROM pk GROUP BY g ORDER BY c DESC, g LIMIT 8');
SELECT * FROM held('SELECT g, min(t), max(t), count(*) AS c FROM pk GROUP BY g ORDER BY g LIMIT 15');
SELECT * FROM held('SELECT k, min(u), max(t), bool_or(b), bool_and(b), count(*) FROM pk GROUP BY k ORDER BY k NULLS FIRST');
SELECT * FROM held('SELECT g, round(stddev_samp(f)::numeric, 6) AS sd, round(var_pop(f)::numeric, 6) AS vp, bool_or(b), min(u) FROM pk GROUP BY g ORDER BY g LIMIT 15');
SELECT * FROM held('SELECT min(u), max(u), min(t), count(*) FROM pk');
-- HAVING keeps its groups out of the heap
SELECT * FROM held('SELECT g, count(*) AS c FROM pk GROUP BY g HAVING count(*) > 9 ORDER BY c DESC, g LIMIT 5');
SELECT * FROM held('SELECT g, min(u) AS m, count(*) AS c FROM pk GROUP BY g HAVING count(*) > 9 ORDER BY c DESC, g LIMIT 5');
-- a filter, and a LIMIT that stops the Gather early
SELECT * FROM held('SELECT g, min(u), count(*) AS c FROM pk WHERE v < 500 AND k > 40 GROUP BY g ORDER BY c DESC, g LIMIT 6');
SELECT count(*) AS limited FROM (SELECT g, min(u), count(*) FROM pk GROUP BY g LIMIT 10) AS q;
DROP FUNCTION held(text), answer_of(text), plan_of(text);
DROP TABLE pk;
DROP EXTENSION pg_sift;
