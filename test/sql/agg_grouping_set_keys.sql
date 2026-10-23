-- Group keys computed inside the nullable input of an outer join (placeholders) under grouping sets: one set leaves a key out, so the
-- grouping step marks the keys nulled; the keys are still the expressions of the input, NULL for the rows no input row matched.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE gs_facts (id int PRIMARY KEY, dim_a_id int, day date, kind text, metrics_a jsonb);
INSERT INTO gs_facts SELECT i, i % 7, date '2024-01-01' + i % 5, (ARRAY['a', 'b', NULL])[1 + i % 3],
	jsonb_build_object('catg_i_catg_a', i % 10, 'catg_i', 3, 'catg_m', i % 4) FROM generate_series(1, 3000) i;
CREATE INDEX gs_facts_col ON gs_facts USING sift_col (id, dim_a_id, day, kind, metrics_a) WITH (paths = 'metrics_a.* float8');
VACUUM (FREEZE, ANALYZE) gs_facts;

\ir test/agg_check_pushed.sql

-- two grouping sets; the key computed in the input is in both, the attribute in one; days nothing matches keep a row
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT f.id, f.day, f.kind, split_part(e.key, '_', 1) AS category, nullif(split_part(e.key, '_', 2), '') AS val,
          CASE WHEN f.dim_a_id % 2 = 0 THEN 'even' ELSE 'odd' END AS attribute, e.value::float8 AS v
   FROM gs_facts f CROSS JOIN LATERAL jsonb_each(f.metrics_a) e
 )
 SELECT coalesce(p.category, 'm_total') AS category, p.val, p.attribute, d.day, count(DISTINCT p.id) AS n, avg(p.v) AS mean
 FROM (VALUES (date '2024-01-01'), (date '2024-01-03'), (date '2030-01-01')) d(day)
 LEFT JOIN parsed p ON p.day = d.day
 GROUP BY GROUPING SETS ((p.category, p.val, p.attribute, d.day), (p.category, p.val, d.day))
$q$);
-- the same with a ROLLUP over the placeholders
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT f.id, f.day, split_part(e.key, '_', 1) AS category, e.value::float8 AS v
   FROM gs_facts f CROSS JOIN LATERAL jsonb_each(f.metrics_a) e
 )
 SELECT p.category, d.day, count(*) AS rows, count(p.id) AS n, sum(p.v) AS total
 FROM (VALUES (date '2024-01-01'), (date '2030-01-01')) d(day)
 LEFT JOIN parsed p ON p.day = d.day
 GROUP BY ROLLUP (p.category, d.day)
$q$);

-- a constant key: the empty set nulls it in the output (before PostgreSQL 18 only a column of the scan tuple is nulled, never a bare constant; from 18 the planner wraps it and the normal plan answers)
SELECT pushed OR current_setting('server_version_num')::int >= 180000 AS pushed, differences FROM check_query($q$
 SELECT 'cur'::text AS label, kind, count(*) FROM gs_facts GROUP BY GROUPING SETS ((1, 2), ()) ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
$q$);
-- a ROLLUP over a constant and a column; a constant that is no key stays the constant, and so does one read through an expression
SELECT pushed OR current_setting('server_version_num')::int >= 180000 AS pushed, differences FROM check_query($q$
 SELECT 'cur'::text AS label, 7 AS seven, kind, 'x' || kind AS tagged, count(*) FROM gs_facts GROUP BY ROLLUP (1, kind) ORDER BY 1 NULLS FIRST, 3 NULLS FIRST
$q$);
-- two constants, one of them the key of a set of its own
SELECT pushed OR current_setting('server_version_num')::int >= 180000 AS pushed, differences FROM check_query($q$
 SELECT 1 AS one, 'cur'::text AS label, count(*) FROM gs_facts GROUP BY GROUPING SETS ((1), (2), ())
$q$);
DROP FUNCTION check_query(text);
DROP TABLE gs_facts;
DROP EXTENSION pg_sift;
