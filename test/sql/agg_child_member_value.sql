-- A correlated child is given the member of a jsonb_each as the jsonb of the float8 the leaf holds, which is not the document's number when the
-- document wrote a scale (12.50): the child reads the member only through casts that float8 serves exactly (integers and floats), as PostgreSQL
-- gives them. A numeric cast, a boolean cast and the jsonb itself keep PostgreSQL's plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, dim_a_id int, metrics jsonb);
INSERT INTO facts
SELECT i, i % 40, CASE i % 3 WHEN 0 THEN '{"k": 12.50, "j": 1.0}'::jsonb WHEN 1 THEN '{"k": 3, "j": 2}'::jsonb ELSE jsonb_build_object('k', i + 0.25, 'j', i) END
FROM generate_series(1, 3000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, metrics) WITH (paths = 'metrics.* float8');
VACUUM (FREEZE, ANALYZE) facts;
CREATE FUNCTION outcome(q text, enabled bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE result text;
BEGIN
  PERFORM set_config('sift.enable_agg', enabled::text, true);
  PERFORM set_config(s, (NOT enabled)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text)::text, ''no rows'') FROM (' || q || ') t' INTO result;
  RETURN result;
EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE || ' ' || SQLERRM;
END $$;
CREATE FUNCTION runs_in_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text; found bool := false;
BEGIN
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := found OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  RETURN found;
END $$;
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true);
END $$;
-- one row per member: the integer and float casts run in the scan
SELECT * FROM check_query('int cast', $q$
 SELECT c.v, count(*) FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON true
 JOIN LATERAL (SELECT r.value::int8 AS v UNION ALL SELECT 1) c ON true GROUP BY 1
$q$);
SELECT * FROM check_query('float cast', $q$
 SELECT c.v, count(*) FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON true
 JOIN LATERAL (SELECT r.value::float8 AS v UNION ALL SELECT 1) c ON true GROUP BY 1
$q$);
SELECT * FROM check_query('key', $q$
 SELECT c.v, count(*) FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON true
 JOIN LATERAL (SELECT r.key AS v UNION ALL SELECT 'x') c ON true GROUP BY 1
$q$);
-- the number with the scale of the document, and the member as it is
SELECT * FROM check_query('numeric cast', $q$
 SELECT c.v, count(*) FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON true
 JOIN LATERAL (SELECT r.value::numeric AS v UNION ALL SELECT 1) c ON true GROUP BY 1
$q$);
SELECT * FROM check_query('jsonb', $q$
 SELECT c.v, count(*) FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON true
 JOIN LATERAL (SELECT r.value AS v UNION ALL SELECT '1'::jsonb) c ON true GROUP BY 1
$q$);
-- the same through the member a LEFT JOIN picks (PostgreSQL 18 runs it in the scan; the results agree whatever the version)
SELECT same FROM check_query('keyed numeric cast', $q$
 SELECT c.v, count(*) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'k'
 JOIN LATERAL (SELECT r.value::numeric AS v UNION ALL SELECT 1) c ON true GROUP BY 1
$q$);
SELECT same FROM check_query('keyed int cast', $q$
 SELECT c.v, count(*) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'k'
 JOIN LATERAL (SELECT r.value::int8 AS v UNION ALL SELECT 1) c ON true GROUP BY 1
$q$);
DROP FUNCTION check_query(text, text), runs_in_scan(text), outcome(text, bool);
DROP TABLE facts;
DROP EXTENSION pg_sift;
