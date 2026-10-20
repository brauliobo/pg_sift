-- A sub-select that reads a statement-level common table expression is a plan read once, not a plan per row.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, dim_a_id int, day date, amount float8, payload text);
INSERT INTO facts
SELECT i, i % 50, date '2026-01-01' + (i % 120), CASE WHEN i % 7 = 0 THEN NULL ELSE i % 13 END, repeat('payload', 50)
FROM generate_series(1, 100000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, day, amount);
VACUUM (FREEZE, ANALYZE) facts;
-- pushed: the plan PostgreSQL costs cheapest; eligible: the plan with its own table scans off, which a Custom Scan (SiftAgg) beats only if the query is one it can run
CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, eligible bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP pushed := pushed OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  SET LOCAL enable_indexonlyscan = off;
  eligible := false;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP eligible := eligible OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
-- Overlapping, duplicate, NULL and empty periods: every matching period multiplies the facts.
SELECT * FROM check_query($q$
 WITH periods AS MATERIALIZED (
  SELECT * FROM (VALUES ('a', daterange('2026-01-10', '2026-02-10', '[]')), ('b', daterange('2026-02-01', '2026-03-20', '[]')),
   ('b', daterange('2026-02-01', '2026-03-20', '[]')), ('c', NULL), ('d', daterange('2030-01-01', '2030-02-01', '[]'))) p (period, range)),
 spans AS (SELECT DISTINCT period, range FROM periods)
 SELECT s.period, count(*) AS n, count(DISTINCT f.id) AS ids, sum(f.amount) AS total
 FROM facts f JOIN spans s ON f.day <@ s.range GROUP BY 1
$q$);
-- Periods preserved by an outer join keep their empty buckets, with duplicate and NULL periods among them.
SELECT * FROM check_query($q$
 WITH periods AS MATERIALIZED (
  SELECT 'm' || m AS period, daterange(date '2026-01-01' + m * 10, date '2026-01-01' + m * 10 + 12, '[]') AS range
  FROM generate_series(0, 14) m
  UNION ALL SELECT 'm0', daterange(date '2026-01-01', date '2026-01-13', '[]')
  UNION ALL SELECT 'empty', daterange('2030-01-01', '2030-02-01', '[]')
  UNION ALL SELECT 'null', NULL),
 spans AS (SELECT DISTINCT period, range FROM periods)
 SELECT s.period, count(f.id) AS n, sum(f.amount) AS total
 FROM spans s LEFT JOIN facts f ON f.day <@ s.range GROUP BY 1
$q$);
-- A common table expression of the sub-select itself is read once as well.
SELECT * FROM check_query($q$
 SELECT s.period, count(*) AS n
 FROM facts f JOIN (
  WITH p AS (SELECT * FROM (VALUES ('a', date '2026-01-10'), ('b', date '2026-02-10')) v (period, since))
  SELECT DISTINCT period, since FROM p) s ON f.day = s.since GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
