-- A WITH inside a derived table (a ranked top-N over an aggregate, a UNION ALL of aggregates over one CTE) is planned like the statement's own: its CTEs are lookup
-- plans of the scan when the derived table reads nothing of the queries around it, and a CTE of it that several aggregates read is planned inline.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, kind text);
INSERT INTO dim_a SELECT i, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4] FROM generate_series(1, 60) i;
CREATE TABLE facts (id int, day date, dim_a_id int, deleted_at timestamp, items jsonb);
INSERT INTO facts
SELECT i, '2026-01-01'::date + (i % 15), 1 + i % 70, CASE WHEN i % 17 = 0 THEN now() END,
       jsonb_build_array(jsonb_build_object('qty', i % 5), jsonb_build_object('qty', i % 3))
FROM generate_series(1, 60000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, day, dim_a_id, deleted_at, items) WITH (paths = 'items[]{qty float8}');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dim_a;
\ir test/agg_check_query.sql
-- The top three kinds of a ranked aggregate whose own WITH holds the lookup tables of the scan.
SELECT * FROM check_query($q$
 SELECT r.kind, r.n, r.rn FROM (
   SELECT t.*, row_number() OVER (ORDER BY t.n DESC, t.kind) AS rn FROM (
     WITH scope AS MATERIALIZED (SELECT DISTINCT id FROM dim_a WHERE kind <> 'd'),
          scoped AS MATERIALIZED (SELECT id, kind FROM dim_a JOIN scope USING (id))
     SELECT e.kind, count(*) AS n FROM facts f JOIN scope s ON s.id = f.dim_a_id JOIN scoped e ON e.id = s.id WHERE f.deleted_at IS NULL GROUP BY e.kind
   ) t
 ) r WHERE r.rn <= 3
$q$);
-- The arms of a UNION ALL read one CTE of the derived table that holds them (the unnest of its rows makes the materialized copy cost most).
SELECT * FROM check_query($q$
 SELECT * FROM (
   WITH scoped AS (SELECT f.id, f.day, e.kind, i.qty FROM facts f JOIN dim_a e ON e.id = f.dim_a_id CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS i(qty float8) WHERE f.deleted_at IS NULL)
   SELECT 'day' AS grp, day::text AS k, count(DISTINCT id) AS n, sum(qty) AS q FROM scoped GROUP BY day
   UNION ALL
   SELECT 'kind', kind, count(DISTINCT id), sum(qty) FROM scoped GROUP BY kind
 ) q
$q$);
-- A WITH inside a lateral sub-select that reads the row of the query around it changes with that row: the CTE is not a lookup of the scan.
SELECT * FROM check_query($q$
 SELECT e.id, x.n FROM dim_a e CROSS JOIN LATERAL (
   WITH mine AS MATERIALIZED (SELECT id FROM facts WHERE dim_a_id = e.id)
   SELECT count(*) AS n FROM facts f JOIN mine m ON m.id = f.id WHERE f.deleted_at IS NULL
 ) x
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, dim_a;
DROP EXTENSION pg_sift;
