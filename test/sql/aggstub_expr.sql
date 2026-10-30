-- Expressions without a kernel on the test column source: PostgreSQL evaluates them over the decoded leaves (integer, date, float and
-- dictionary leaves) as aggregate arguments, group keys and filters, with the rows of the normal plan (sift.enable_agg off); a division
-- that the normal plan never reaches does not raise, and a small combo cache that is dropped many times changes nothing.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION pg_sift_test;
LOAD 'pg_sift';

CREATE TABLE e (id int, g int, b int8, d date, f float8, t text, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO e
SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 100003 - 50000 END, date '2024-01-01' + i % 400,
	   CASE WHEN i % 13 = 0 THEN NULL WHEN i % 29 = 0 THEN 'NaN' ELSE (i % 64) / 4.0 - 3 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || (i % 9) END, NULL
FROM generate_series(1, 6000) i;
CREATE INDEX e_col ON e USING sift_colstub (id, g, b, d, f, t);
ANALYZE e;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;

EXPLAIN (COSTS OFF) SELECT g, sum(f / 3), max(b % 7), count(id / 2) FROM e GROUP BY g;
EXPLAIN (COSTS OFF) SELECT b % 7 AS k, date_trunc('month', d) AS m, count(*) FROM e GROUP BY 1, 2;
EXPLAIN (COSTS OFF) SELECT g, count(*) FILTER (WHERE sqrt(abs(f)) > 1 AND g <> 0 AND id / g > 3) FROM e GROUP BY g;
-- an integer sum PostgreSQL would evaluate: a generic aggregate
EXPLAIN (COSTS OFF) SELECT g, sum(id / 2) FROM e GROUP BY g;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal_nonempty(q) $$;
SELECT same($$SELECT g, sum(f / 3) AS a, max(b % 7) AS b, min(b / 3) AS c, count(id / 2) AS d, max(sqrt(abs(f))) AS e FROM e GROUP BY g$$) AS arguments;
SELECT same($$SELECT b % 7 AS k, date_trunc('month', d) AS m, upper(t) AS u, count(*) AS n, max(id % 100) AS x FROM e GROUP BY 1, 2, 3$$) AS keys;
SELECT same($$SELECT g, count(*) FILTER (WHERE sqrt(abs(f)) > 1) AS a, count(*) FILTER (WHERE b / 3 > 100 AND d > date '2024-06-01') AS b,
			  count(*) FILTER (WHERE id % 5 = ANY (ARRAY[1, 2]::int8[])) AS c, count(*) FILTER (WHERE f::numeric > 1.5) AS e FROM e GROUP BY g$$) AS filters;
-- the divisor is zero for g = 0 and b = 0, in rows the guard excludes
SELECT same($$SELECT g, sum(id / g) FILTER (WHERE g <> 0) AS a, count(*) FILTER (WHERE b <> 0 AND 1000 / b > 2) AS b, max(CASE WHEN g = 0 THEN 0 ELSE 50 / g END) AS c FROM e GROUP BY g$$) AS guarded;
SELECT same($$SELECT count(*) FROM e WHERE g <> 0 AND id / g > 3$$) AS guarded_where;
SET sift.agg_combo_cache = 64;
SELECT same($$SELECT g, sum(f / 3) AS a, max(b % 7 + id % 13) AS b FROM e GROUP BY g$$) AS combo_cache;
RESET sift.agg_combo_cache;
SELECT same($$SELECT g, sum(id / 2) AS a, avg(b % 4) AS b FROM e GROUP BY g$$) AS int_sums;

DROP FUNCTION same(text);
DROP TABLE e;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
