-- Predicate-only joins to child plans keep overlapping and duplicate matches.
-- Use default planner costs: both parity and natural column aggregation matter.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, day date, metrics jsonb);
INSERT INTO events
SELECT i, date '2024-01-01' + i % 60,
       CASE WHEN i % 11 = 0 THEN '{}'::jsonb ELSE jsonb_build_object('overall', i % 11) END
FROM generate_series(1, 50000) i;
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
 SELECT d.day, coalesce(p.key, 'overall') AS category, count(*) AS n,
        count(DISTINCT p.id) AS ids, avg(p.value) AS value,
        count(DISTINCT p.id) FILTER (WHERE p.value >= 8) AS promoters
 FROM (VALUES (date '2024-01-01'), (date '2024-01-02'),
              (date '2024-05-01'), (NULL::date), (date '2024-01-01')) d(day)
 LEFT JOIN parsed p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY d.day, p.key
$q$);
-- Filters in the nullable input affect matches; they must retain the empty bucket.
SELECT * FROM check_query($q$
 SELECT d.day, coalesce(p.key, 'overall') AS category, count(*) AS n,
        count(DISTINCT p.id) AS ids, avg(p.value) AS value
 FROM (VALUES (date '2024-01-01'), (date '2024-01-02'), (date '2024-05-01')) d(day)
 LEFT JOIN (
   SELECT e.id, e.day, r.key, r.value::float8 AS value
   FROM events e CROSS JOIN LATERAL jsonb_each(e.metrics) r
   WHERE e.id < 20000 AND r.value::float8 > 8
 ) p ON jsonb_build_array(p.day) = jsonb_build_array(d.day)
 GROUP BY GROUPING SETS ((d.day, p.key), (d.day))
$q$);
DROP FUNCTION check_query(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
