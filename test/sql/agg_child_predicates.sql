-- Predicate-only joins to child plans keep overlapping and duplicate matches.
-- Use default planner costs: both parity and natural column aggregation matter.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, day date, metrics jsonb);
INSERT INTO events
SELECT i, date '2024-01-01' + i % 60,
       CASE WHEN i % 11 = 0 THEN '{}'::jsonb ELSE jsonb_build_object('mhead', i % 11) END
FROM generate_series(1, 50000) i;
INSERT INTO events VALUES (50001, NULL, '{"mhead":8}');
CREATE INDEX events_col ON events USING sift_col (id, day, metrics) WITH (paths = 'metrics.* float8');
VACUUM (FREEZE, ANALYZE) events;
CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM
    ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
SELECT * FROM check_query($q$
 SELECT p.label, count(*) AS n, count(DISTINCT e.id) AS ids, avg(r.value::float8) AS value
 FROM events e
 JOIN (VALUES ('first', daterange('2024-01-01', '2024-02-01')),
              ('overlap', daterange('2024-01-15', '2024-03-01')),
              ('first', daterange('2024-01-01', '2024-02-01')),
              ('empty', daterange('2025-01-01', '2025-02-01')),
              ('null', NULL::daterange)) p(label, span) ON e.day <@ p.span
 CROSS JOIN LATERAL jsonb_each(e.metrics) r
 GROUP BY p.label
$q$);
SELECT * FROM check_query($q$
 SELECT p.label, count(*) AS n, count(e.id) AS ids
 FROM events e
 LEFT JOIN (VALUES ('first', daterange('2024-01-01', '2024-02-01')),
                   ('overlap', daterange('2024-01-15', '2024-02-01')),
                   ('null', NULL::daterange)) p(label, span) ON e.day <@ p.span
 GROUP BY p.label
$q$);
-- A date bucket with no parsed facts still contributes one NULL fact input.
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT e.id, e.day, r.key, r.value::float8 AS value
   FROM events e CROSS JOIN LATERAL jsonb_each(e.metrics) r
 )
 SELECT d.day, coalesce(p.key, 'mhead') AS category, count(*) AS n,
        count(DISTINCT p.id) AS ids, avg(p.value) AS value,
        count(DISTINCT p.id) FILTER (WHERE p.value >= 8) AS promoters
 FROM (VALUES (date '2024-01-01'), (date '2024-01-02'),
              (date '2024-05-01'), (NULL::date), (date '2024-01-01')) d(day)
 LEFT JOIN parsed p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY d.day, p.key
$q$);
-- Filters in the nullable input affect matches; they must retain the empty bucket.
SELECT * FROM check_query($q$
 SELECT d.day, coalesce(p.key, 'mhead') AS category, count(*) AS n,
        count(DISTINCT p.id) AS ids, avg(p.value) AS value
 FROM (VALUES (date '2024-01-01'), (date '2024-01-02'), (date '2024-05-01')) d(day)
 LEFT JOIN (
   SELECT e.id, e.day, r.key, r.value::float8 AS value
   FROM events e CROSS JOIN LATERAL jsonb_each(e.metrics) r
   WHERE e.id < 20000 AND r.value::float8 > 8
 ) p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY GROUPING SETS ((d.day, p.key), (d.day))
$q$);
-- Preserve buckets around several nullable-input lookups and their filters.
CREATE TABLE dim_a (id int, label text);
INSERT INTO dim_a SELECT i, 'g' || i % 3 FROM generate_series(0, 99) i;
INSERT INTO dim_a VALUES (1, 'duplicate');
CREATE TABLE labels (key text, category text);
INSERT INTO labels VALUES ('mhead', 'quality'), ('mhead', 'quality'), ('unused', NULL);
ANALYZE dim_a; ANALYZE labels;
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT f.id, f.day, e.label, coalesce(m.category, r.key) AS category,
          r.value::float8 AS value
   FROM events f JOIN dim_a e ON e.id = f.id % 100
   CROSS JOIN LATERAL jsonb_each(f.metrics) r
   LEFT JOIN labels m ON m.key = r.key
   WHERE f.id < 40000 AND e.label <> 'g2'
 )
 SELECT d.day, p.label, coalesce(p.category, 'mhead') AS category,
        count(*) AS n, count(DISTINCT p.id) AS ids, avg(p.value) AS value
 FROM (VALUES (date '2024-01-01'), (date '2024-01-02'),
              (date '2024-05-01'), (NULL::date), (date '2024-01-01')) d(day)
 LEFT JOIN parsed p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY d.day, p.label, p.category
$q$);
-- An expression inside the nullable input differs from its outside default.
-- A matched NULL day and an unmatched bucket share NULL source leaves but have different placeholder values.
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT f.id, f.day, coalesce(f.day, date '2000-01-01') AS label
   FROM events f CROSS JOIN LATERAL jsonb_each(f.metrics) r
 )
 SELECT d.day, coalesce(p.label, date '3000-01-01') AS label, count(*) AS n, count(p.id) AS ids
 FROM (VALUES (NULL::date), (date '2024-05-01'), (date '2024-01-01')) d(day)
 LEFT JOIN parsed p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY d.day, p.label
$q$);
-- A predicate after preservation must still see the NULL lookup columns of an unmatched bucket.
SELECT * FROM check_query($q$
 SELECT d.day, count(*) AS n, count(p.id) AS ids
 FROM (VALUES (date '2024-01-01'), (date '2024-05-01')) d(day)
 LEFT JOIN (SELECT f.id, f.day, e.label FROM events f JOIN dim_a e ON e.id = f.id % 100) p
 ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 WHERE p.label IS NULL GROUP BY d.day
$q$);
-- Constants projected inside the nullable input have no source leaf, but still have a nulling boundary.
SELECT * FROM check_query($q$
 SELECT d.day, p.dimension, coalesce(p.bucket_kind, 'empty') AS kind,
        count(*) AS n, count(DISTINCT p.id) AS ids
 FROM (VALUES (NULL::date), (date '2024-05-01'), (date '2024-01-01')) d(day)
 LEFT JOIN (
   SELECT f.id, f.day, NULL::jsonb AS dimension, 'base'::text AS bucket_kind
   FROM events f CROSS JOIN LATERAL jsonb_each(f.metrics) r
 ) p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY d.day, p.dimension, p.bucket_kind
$q$);
-- PostgreSQL removes an unused unique LEFT lookup; SiftAgg must use the surviving relations as well.
CREATE TABLE auxiliary (id int PRIMARY KEY);
SELECT * FROM check_query($q$
 SELECT r.key, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f LEFT JOIN auxiliary a ON a.id = f.id
 CROSS JOIN LATERAL jsonb_each(f.metrics) r GROUP BY r.key
$q$);
DROP TABLE auxiliary;
DROP TABLE dim_a, labels;
DROP FUNCTION check_query(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
