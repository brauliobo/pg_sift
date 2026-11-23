-- COUNT(DISTINCT integer column) in the parallel SiftAggV6: a participant hands the values of its rows to the partitions as records (a query of no keys dedups them in a flat set first), the owner of a
-- partition counts them in a flat set of its own, and a set that outgrows its memory moves to the exact generic set: every answer of a parallel run equals the heap's own aggregate.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 3;
CREATE TABLE dl AS
SELECT i, ((i * 7919) % 40009)::int8 AS g, (i * 31 % 1000)::int8 AS v, (i * 17 % 211)::int4 AS r, (i * 13 % 23)::int2 AS s, 's' || (i * 13 % 5003) AS t,
       i % 3 = 0 AS b, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7 % 97)::int END AS k, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 101 % 50021)::int8 END AS n, ((i * 17) % 700)::float8 AS f
FROM generate_series(1::int8, 400000::int8) AS i;
CREATE INDEX dl_col ON dl USING sift_col (g, v, r, s, t, b, k, n, f);
VACUUM (ANALYZE, FREEZE) dl;
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
-- a query of no keys: one value column, several, of every integer width, with NULLs, beside the other aggregates
SELECT * FROM held('SELECT count(DISTINCT g) FROM dl');
SELECT * FROM held('SELECT count(DISTINCT n), count(DISTINCT k), count(DISTINCT s), count(DISTINCT b) FROM dl');
SELECT * FROM held('SELECT count(DISTINCT g), count(DISTINCT v), count(*), sum(v) FROM dl');
SELECT * FROM held('SELECT count(DISTINCT r) FROM dl WHERE v < 700 AND k IS NOT NULL');
SELECT * FROM held('SELECT count(DISTINCT g) FILTER (WHERE b), count(DISTINCT v) FROM dl');
-- one key of each kind: an integer, two integers that pack into one inline key, a text, a text and an integer, a key with NULLs
SELECT * FROM held('SELECT r, count(DISTINCT g) AS d, count(*) AS c FROM dl GROUP BY r ORDER BY r');
SELECT * FROM held('SELECT s, r, count(DISTINCT g) AS d, count(*) FROM dl GROUP BY s, r ORDER BY s, r');
SELECT * FROM held('SELECT t, count(DISTINCT g) AS d, count(*) AS c FROM dl GROUP BY t ORDER BY t');
SELECT * FROM held('SELECT t, s, count(DISTINCT n) AS d, sum(v), count(*) FROM dl GROUP BY t, s ORDER BY t, s');
SELECT * FROM held('SELECT k, count(DISTINCT g) AS d, count(DISTINCT v) AS e FROM dl GROUP BY k ORDER BY k NULLS FIRST');
SELECT * FROM held('SELECT g, count(DISTINCT v) AS d, count(*) FROM dl GROUP BY g ORDER BY g');
-- the values and the groups together: filters, HAVING, a count of a column with NULLs, a boolean, an aggregate of text beside the distinct count
SELECT * FROM held('SELECT r, count(DISTINCT n) AS d FROM dl WHERE v < 500 GROUP BY r HAVING count(DISTINCT n) > 50 ORDER BY d DESC, r LIMIT 8');
SELECT * FROM held('SELECT s, count(DISTINCT b) AS d, count(DISTINCT k) AS e, count(*) FROM dl GROUP BY s ORDER BY s');
SELECT * FROM held('SELECT r, count(DISTINCT g) FILTER (WHERE b) AS d, count(DISTINCT v) AS e FROM dl GROUP BY r ORDER BY r LIMIT 20');
SELECT * FROM held('SELECT s, count(DISTINCT g) AS d, min(t) AS m FROM dl GROUP BY s ORDER BY s');
-- the shapes this path leaves to the generic set: text, float, and a row of columns
SELECT * FROM held('SELECT r, count(DISTINCT t) AS d FROM dl GROUP BY r ORDER BY r LIMIT 10');
SELECT * FROM held('SELECT s, count(DISTINCT f) AS d FROM dl GROUP BY s ORDER BY s');
SELECT * FROM held('SELECT s, count(DISTINCT (g, v)) AS d FROM dl GROUP BY s ORDER BY s');
-- the memory of the sets and of the partitions is as small as it can be: the sets move to the exact generic set and the records go to files
SET work_mem = '64kB';
SELECT * FROM held('SELECT count(DISTINCT g), count(DISTINCT n) FROM dl');
SELECT * FROM held('SELECT r, count(DISTINCT g) AS d, count(DISTINCT n) AS e, count(*) FROM dl GROUP BY r ORDER BY r');
SELECT * FROM held('SELECT t, count(DISTINCT g) AS d, count(*) FROM dl GROUP BY t ORDER BY t');
SELECT * FROM held('SELECT s, r, count(DISTINCT n) AS d, count(*) FROM dl GROUP BY s, r ORDER BY s, r');
RESET work_mem;
-- a LIMIT with no ORDER BY, and a rescan of the node
SELECT parallel, same FROM held('SELECT r, count(DISTINCT g) FROM dl GROUP BY r ORDER BY r LIMIT 3');
SELECT * FROM held('SELECT x.r, x.d FROM (SELECT 1 AS a UNION ALL SELECT 2) y, LATERAL (SELECT r, count(DISTINCT g) AS d FROM dl WHERE s = y.a GROUP BY r ORDER BY d DESC, r LIMIT 2) x ORDER BY 1, 2');
DROP FUNCTION held(text), answer_of(text), plan_of(text);
DROP TABLE dl;
DROP EXTENSION pg_sift;
