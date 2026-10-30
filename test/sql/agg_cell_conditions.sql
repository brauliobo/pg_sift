-- The outer aggregates of a two-level scan with a FILTER on the value of a cell (an inner average, sum or count) read that value once for the cell and compare it with the
-- constant inline: NaN above every number and equal to itself, infinities, NULL cells (an average of no value), integer and float comparisons. The rows are the normal plan's.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, cat int, who int, x float8, k int) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, i % 5, i % 211,
       CASE WHEN i % 17 = 0 THEN NULL WHEN i % 101 = 0 THEN 'NaN' WHEN i % 103 = 0 THEN 'Infinity' WHEN i % 107 = 0 THEN '-Infinity' ELSE (hashint4(i) % 2000) / 100.0 END,
       i % 9
FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (cat, who, x, k);
VACUUM (FREEZE, ANALYZE) ev;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_rows_and_differences(q) $$;

SELECT * FROM same($q$SELECT cat, count(*) AS cells, count(*) FILTER (WHERE a >= 8) AS hi, count(*) FILTER (WHERE a < 6) AS lo, count(*) FILTER (WHERE a > 6 AND a < 8) AS mid,
  count(*) FILTER (WHERE a = 'NaN') AS nan, count(*) FILTER (WHERE a <> 'NaN') AS not_nan, count(*) FILTER (WHERE a <= 'Infinity') AS le_inf, count(a) AS with_a
  FROM (SELECT cat, who, avg(x) AS a FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
SELECT * FROM same($q$SELECT cat, count(*) AS cells, sum(n) FILTER (WHERE n > 100) AS big, count(*) FILTER (WHERE n = 190) AS exact, count(*) FILTER (WHERE s >= 3000 AND n < 190) AS mixed, max(s) AS m
  FROM (SELECT cat, who, count(*) AS n, sum(k) AS s FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
SELECT * FROM same($q$SELECT cat, avg(a) FILTER (WHERE a > 5) AS m1, round((sum(a) FILTER (WHERE a < 5 AND a > -5))::numeric, 6) AS m2, count(a) FILTER (WHERE a >= 'NaN') AS m3
  FROM (SELECT cat, who, avg(x) FILTER (WHERE k > 2) AS a FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
EXPLAIN (COSTS OFF) SELECT cat, count(*) FILTER (WHERE a >= 8) FROM (SELECT cat, who, avg(x) AS a FROM ev GROUP BY 1, 2) c GROUP BY 1;
SET sift.agg_force = on;
SELECT * FROM same($q$SELECT cat, count(*) FILTER (WHERE a >= 8) AS hi, count(*) FILTER (WHERE a = 'NaN') AS nan, sum(n) AS total
  FROM (SELECT cat, who, avg(x) AS a, count(*) AS n FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
RESET sift.agg_force;

DROP FUNCTION same(text);
DROP TABLE ev;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
