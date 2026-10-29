-- A function or UNION that reads a joined row fans it out; the scan runs PostgreSQL's plan of the child for each distinct input.
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, labels text[], region text);
INSERT INTO dim_a SELECT i, CASE i % 5 WHEN 0 THEN NULL WHEN 1 THEN ARRAY[]::text[] WHEN 2 THEN ARRAY['one', 'one', 'two'] WHEN 3 THEN ARRAY['two', NULL] ELSE ARRAY['three'] END,
 CASE WHEN i % 4 = 0 THEN 'north' ELSE 'south' END FROM generate_series(0, 49) i;
CREATE TABLE facts (id int, dim_a_id int, amount float8, metrics jsonb, code text, payload text);
INSERT INTO facts
SELECT i, CASE WHEN i % 101 = 0 THEN NULL ELSE i % 52 END, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 13 END, jsonb_build_object('m_total', i % 10), CASE WHEN i % 101 = 0 THEN 'none' ELSE (i % 10)::text END, repeat('payload', 50)
FROM generate_series(1, 60000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, amount, metrics, code) WITH (paths = 'metrics.* float8');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dim_a;
\ir test/agg_check_query.sql
-- A UNION ALL of arms, one reading the item: every arm contributes a row, NULL items included.
SELECT * FROM check_query($q$
 SELECT c.category, count(*) AS n, count(DISTINCT f.id) AS ids, sum(f.amount) AS total
 FROM facts f JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN f.dim_a_id > 25 THEN 'high' ELSE 'low' END) c ON true
 GROUP BY 1
$q$);
-- UNION removes the arms' duplicates per item (a high item is 'all' and 'high'; here both arms agree for some items).
SELECT * FROM check_query($q$
 SELECT c.category, count(DISTINCT f.id) AS ids
 FROM facts f JOIN LATERAL (SELECT 'x' AS category UNION SELECT CASE WHEN f.dim_a_id > 25 THEN 'x' ELSE 'low' END) c ON true
 GROUP BY 1
$q$);
-- A set-returning function of the item: no row for a non-positive count with the inner join, a NULL row with the left join.
SELECT * FROM check_query($q$
 SELECT g, count(*) AS n FROM facts f CROSS JOIN LATERAL generate_series(1, f.dim_a_id % 4) g GROUP BY g
$q$);
SELECT * FROM check_query($q$
 SELECT g, count(*) AS n, count(f.id) AS ids FROM facts f LEFT JOIN LATERAL generate_series(1, f.dim_a_id % 4) g ON true GROUP BY g
$q$);
-- The rows the scan keeps are those of the child for every distinct input: more than sift.agg_lookup_max_rows estimated keep the normal plan.
SET sift.agg_lookup_max_rows = 1000;
SELECT * FROM check_query($q$
 SELECT g, count(*) AS n FROM facts f CROSS JOIN LATERAL generate_series(1, f.dim_a_id % 4) g GROUP BY g
$q$);
RESET sift.agg_lookup_max_rows;
-- The array of a joined row fans out: duplicates stay, NULL and empty arrays and NULL elements behave as PostgreSQL does.
SELECT * FROM check_query($q$
 SELECT label, count(*) AS n, count(DISTINCT f.id) AS ids FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
 LEFT JOIN LATERAL unnest(e.labels) label ON true GROUP BY label
$q$);
SELECT * FROM check_query($q$
 SELECT e.region, label, count(*) AS n FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
 CROSS JOIN LATERAL unnest(e.labels) label GROUP BY e.region, label
$q$);
-- Stays native: the fan-out casts a joined text column, which can raise ('none' is the code of the rows without an entity, that the join leaves out), so it
-- is not computed for the rows that PostgreSQL's plan has not joined yet.
SELECT * FROM check_query($q$
 SELECT c.category, count(*) AS n
 FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
 JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN f.code::int > 6 THEN 'high' ELSE 'low' END) c ON true
 GROUP BY 1
$q$);
-- The same fan-out on a comparison of the text, which cannot raise, is joined by the scan.
SELECT * FROM check_query($q$
 SELECT c.category, count(*) AS n
 FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
 JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN f.code > '6' THEN 'high' ELSE 'low' END) c ON true
 GROUP BY 1
$q$);
-- The fan-out reads the member of a LEFT JOIN LATERAL jsonb_each on a constant key: a row level value of the index (agg_left_each).
SELECT * FROM check_query($q$
 SELECT c.category, count(*) AS n
 FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
 JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, dim_a;
DROP EXTENSION pg_sift;
