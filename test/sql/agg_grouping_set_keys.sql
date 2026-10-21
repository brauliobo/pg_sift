-- Group keys computed inside the nullable input of an outer join (placeholders) under grouping sets: one set leaves a key out, so the
-- grouping step marks the keys nulled; the keys are still the expressions of the input, NULL for the rows no input row matched.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE gs_facts (id int PRIMARY KEY, entity_id int, day date, kind text, scores jsonb);
INSERT INTO gs_facts SELECT i, i % 7, date '2024-01-01' + i % 5, (ARRAY['a', 'b', NULL])[1 + i % 3],
	jsonb_build_object('asp_i_asp_a', i % 10, 'asp_i', 3, 'asp_m', i % 4) FROM generate_series(1, 3000) i;
CREATE INDEX gs_facts_col ON gs_facts USING sift_col (id, entity_id, day, kind, scores) WITH (paths = 'scores.* float8');
VACUUM (FREEZE, ANALYZE) gs_facts;

CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

-- two grouping sets; the key computed in the input is in both, the dimension in one; days nothing matches keep a row
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT f.id, f.day, f.kind, split_part(e.key, '_', 1) AS category, nullif(split_part(e.key, '_', 2), '') AS val,
          CASE WHEN f.entity_id % 2 = 0 THEN 'even' ELSE 'odd' END AS dimension, e.value::float8 AS v
   FROM gs_facts f CROSS JOIN LATERAL jsonb_each(f.scores) e
 )
 SELECT coalesce(p.category, 'overall') AS category, p.val, p.dimension, d.day, count(DISTINCT p.id) AS n, avg(p.v) AS mean
 FROM (VALUES (date '2024-01-01'), (date '2024-01-03'), (date '2030-01-01')) d(day)
 LEFT JOIN parsed p ON p.day = d.day
 GROUP BY GROUPING SETS ((p.category, p.val, p.dimension, d.day), (p.category, p.val, d.day))
$q$);
-- the same with a ROLLUP over the placeholders
SELECT * FROM check_query($q$
 WITH parsed AS (
   SELECT f.id, f.day, split_part(e.key, '_', 1) AS category, e.value::float8 AS v
   FROM gs_facts f CROSS JOIN LATERAL jsonb_each(f.scores) e
 )
 SELECT p.category, d.day, count(*) AS rows, count(p.id) AS n, sum(p.v) AS total
 FROM (VALUES (date '2024-01-01'), (date '2030-01-01')) d(day)
 LEFT JOIN parsed p ON p.day = d.day
 GROUP BY ROLLUP (p.category, d.day)
$q$);
DROP FUNCTION check_query(text);
DROP TABLE gs_facts;
DROP EXTENSION pg_sift;
