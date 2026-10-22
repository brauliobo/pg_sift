-- A correlated child that reads the value of a jsonb_each member (a cast that can raise) runs only for the members a row reaches, with the
-- error of the value it was given; `x IS TRUE` over an existence test selects the rows the test does.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, dim_a_id int, metrics jsonb, tags_a jsonb);
INSERT INTO facts
SELECT i, i % 40,
       CASE i % 9 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{"m_total": 8}'::jsonb WHEN 2 THEN '{"other": 3}'::jsonb WHEN 3 THEN '{"m_total": 6.5, "other": 9}'::jsonb
                  WHEN 4 THEN NULL ELSE jsonb_build_object('m_total', i % 10, 'other', i % 3) END,
       CASE i % 4 WHEN 0 THEN NULL WHEN 1 THEN '[]'::jsonb WHEN 2 THEN '[{"type":"type_a"}]'::jsonb ELSE '[{"type":"type_c"},{"type":"type_a"}]'::jsonb END
FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, metrics, tags_a) WITH (paths = 'metrics.* float8, tags_a[]{type text}');
VACUUM (FREEZE, ANALYZE) facts;
-- The result, or the SQLSTATE and message, with the scan off and on. on: the table scans are off, so a Custom Scan (SiftAgg) runs when the query is one it can run.
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
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 100);
END $$;
-- A UNION arm casts the value of the member the join kept: numbers round as PostgreSQL's cast does (6.5).
SELECT * FROM check_query('union', $q$
 SELECT c.category, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
 JOIN LATERAL (SELECT 'all' AS category UNION (SELECT CASE WHEN CAST(r.value AS integer) > 6 THEN 'high' ELSE 'low' END)) c ON true
 GROUP BY 1
$q$);
SELECT * FROM check_query('union all, filter, key', $q$
 SELECT c.category, f.dim_a_id % 3 AS k, count(*) AS n
 FROM facts f CROSS JOIN LATERAL jsonb_each(f.metrics) r
 JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
 WHERE f.dim_a_id < 30 AND r.key = 'm_total'
 GROUP BY 1, 2
$q$);
-- a cast to numeric keeps the scale the document wrote, which the stored float does not: the normal plan (agg_child_member_value)
SELECT * FROM check_query('numeric', $q$
 SELECT c.v, count(*) AS n
 FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
 JOIN LATERAL (SELECT r.value::numeric AS v UNION ALL SELECT r.value::numeric + 1) c ON true
 GROUP BY 1
$q$);
-- `x IS TRUE` over an existence test counts the rows the test selects, with or without a join
SELECT * FROM check_query('is true', $q$
 SELECT count(DISTINCT f.id) FILTER (WHERE jsonb_path_exists(f.tags_a, '$[*] ? (@.type != "type_a")') IS TRUE) AS with_comments,
        count(DISTINCT f.id) FILTER (WHERE jsonb_path_exists(f.tags_a, '$[*] ? (@.type == "type_a")') IS TRUE) AS with_responses
 FROM facts f
$q$);
SELECT * FROM check_query('is true, unnest', $q$
 SELECT f.dim_a_id % 5 AS k, count(*) FILTER (WHERE jsonb_path_exists(f.tags_a, '$[*] ? (@.type == "type_a")') IS TRUE) AS n
 FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'other' GROUP BY 1
$q$);
-- Error timing: a member that cannot be cast raises only for the rows that reach the cast, with PostgreSQL's SQLSTATE and message.
INSERT INTO facts VALUES (20001, 1, '{"m_total": 3000000000}', NULL), (20002, 2, '{"m_total": 1e30}', NULL);
VACUUM (FREEZE, ANALYZE) facts;
SELECT * FROM check_query('excluded row', $q$SELECT c.category, count(*) AS n FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id < 20000 GROUP BY 1$q$);
SELECT * FROM check_query('other key', $q$SELECT c.category, count(*) AS n FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'other'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   GROUP BY 1$q$);
SELECT * FROM check_query('int out of range', $q$SELECT c.category, count(*) AS n FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id = 20001 GROUP BY 1$q$);
SELECT * FROM check_query('bigint out of range', $q$SELECT c.category, count(*) AS n FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::bigint > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id = 20002 GROUP BY 1$q$);
-- A member the index cannot hold as a number (a string, an object) keeps the query native, which raises the same errors.
INSERT INTO facts VALUES (20003, 3, '{"m_total": "abc"}', NULL), (20004, 4, '{"m_total": {"a": 1}}', NULL);
VACUUM (FREEZE, ANALYZE) facts;
SELECT * FROM check_query('string member', $q$SELECT c.category, count(*) AS n FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id = 20003 GROUP BY 1$q$);
SELECT * FROM check_query('object member', $q$SELECT c.category, count(*) AS n FROM facts f JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id = 20004 GROUP BY 1$q$);
DROP FUNCTION check_query(text, text), outcome(text, bool), runs_in_scan(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
