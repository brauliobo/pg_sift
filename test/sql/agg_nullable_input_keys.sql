-- A group key computed inside the nullable input of an outer join is NULL for the preserved rows, also when it tests a sub-select (IN is hashed).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dates (day date PRIMARY KEY);
INSERT INTO dates SELECT d::date FROM generate_series('2026-01-01'::date, '2026-01-20', '1 day') d;
CREATE TABLE facts (id int, day date, country text, amount float8, payload text);
INSERT INTO facts
SELECT i, '2026-01-01'::date + (i % 15), CASE WHEN i % 13 = 0 THEN NULL ELSE (ARRAY['pt', 'br', 'us', 'zz'])[1 + i % 4] END, i % 11, repeat('payload', 50)
FROM generate_series(1, 60000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, day, country, amount);
CREATE TABLE countries (iso text);
INSERT INTO countries VALUES ('pt'), ('br'), ('us'), ('pt');
CREATE TABLE countries_null (iso text);
INSERT INTO countries_null VALUES ('pt'), (NULL);
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dates;
ANALYZE countries;
ANALYZE countries_null;
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
-- The dimension of the day's facts as a JSON value, NULL for a country outside the list (and for a day without facts).
SELECT * FROM check_query($q$
 SELECT d.day, f.dimension, count(f.id) AS n, sum(f.amount) AS total
 FROM dates d LEFT JOIN (SELECT id, day, amount, CASE WHEN country IN (SELECT iso FROM countries) THEN jsonb_build_object('iso2', country, 'type', 'country') ELSE NULL END AS dimension FROM facts) f ON f.day = d.day
 GROUP BY 1, 2
$q$);
-- The list holds a NULL: IN is NULL, not false, for the countries it lacks.
SELECT * FROM check_query($q$
 SELECT d.day, f.dimension, count(f.id) AS n
 FROM dates d LEFT JOIN (SELECT id, day, CASE WHEN country IN (SELECT iso FROM countries_null) THEN 'listed' WHEN country NOT IN (SELECT iso FROM countries_null) THEN 'other' ELSE 'unknown' END AS dimension FROM facts) f ON f.day = d.day
 GROUP BY 1, 2
$q$);
-- Without the calendar the key is the plain expression of one column.
SELECT * FROM check_query($q$
 SELECT dimension, count(*) AS n
 FROM (SELECT CASE WHEN country IN (SELECT iso FROM countries) THEN jsonb_build_object('iso2', country, 'type', 'country') ELSE NULL END AS dimension FROM facts) f
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, dates, countries, countries_null;
DROP EXTENSION pg_sift;
