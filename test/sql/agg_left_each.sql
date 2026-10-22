-- `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` is the member k of each row: a row without it stays, with NULL columns. Its value is read at the
-- row level (or by a correlated child through casts), with PostgreSQL's results and errors; before PostgreSQL 18 the cast of a JSON null raises, so a unit that may hold one keeps the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, dim_a_id int, metrics jsonb);
INSERT INTO facts
SELECT i, i % 40,
       CASE i % 7 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{"other": 3}'::jsonb WHEN 2 THEN NULL WHEN 3 THEN '{"m_total": 6.5, "other": 9}'::jsonb
                  ELSE jsonb_build_object('m_total', i % 10, 'other', i % 3) END
FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, metrics) WITH (paths = 'metrics.* float8');
VACUUM (FREEZE, ANALYZE) facts;
-- The result, or the SQLSTATE and message, with the scan off and on. on: the table scans are off, so a Custom Scan (SiftAgg) runs when the query is one it can run.
\ir test/agg_helpers.sql
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 100);
END $$;
-- the rows without the member are counted, with NULL columns
SELECT * FROM check_query('count', $q$
 SELECT f.dim_a_id % 3 AS k, count(*) AS rows, count(CAST(r.value AS float)) AS dim_b, sum(CAST(r.value AS float)) AS total
 FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total' GROUP BY 1
$q$);
SELECT * FROM check_query('filter', $q$
 SELECT count(*) AS n, avg(CAST(r.value AS float)) AS mean
 FROM facts f LEFT JOIN LATERAL (SELECT key, value FROM jsonb_each(f.metrics)) r ON r.key = 'm_total' WHERE f.dim_a_id < 30
$q$);
-- a correlated child reads the member through casts: a row without it gives the child NULL
SELECT * FROM check_query('child', $q$
 SELECT c.category, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM facts f LEFT JOIN LATERAL (SELECT key, value FROM jsonb_each(f.metrics)) ra ON ra.key = 'm_total'
 JOIN LATERAL (SELECT 'all' AS category UNION (SELECT CASE WHEN CAST(ra.value AS integer) > 6 THEN 'high' ELSE 'low' END)) c ON true
 GROUP BY 1
$q$);
-- a child that reads the member another way, and a read of the key: the normal plan
SELECT * FROM check_query('child text', $q$
 SELECT c.v, count(*) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
 JOIN LATERAL (SELECT r.value::text AS v UNION ALL SELECT 'x') c ON true GROUP BY 1
$q$);
SELECT * FROM check_query('key', $q$
 SELECT r.key, count(*) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total' GROUP BY 1
$q$);
SELECT * FROM check_query('child union all, bare read', $q$
 SELECT c.v, count(*) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) ra ON ra.key = 'm_total'
 JOIN LATERAL (SELECT 'a' AS v UNION ALL SELECT (ra.value IS NULL)::text) c ON true GROUP BY 1
$q$);
-- Error timing: the cast of the member raises for the rows that reach it, with PostgreSQL's SQLSTATE and message.
INSERT INTO facts VALUES (20001, 1, '{"m_total": 3000000000}'), (20002, 2, '{"m_total": 1e30}');
VACUUM (FREEZE, ANALYZE) facts;
SELECT * FROM check_query('excluded row', $q$SELECT c.category, count(*) AS n FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id < 20000 GROUP BY 1$q$);
SELECT * FROM check_query('int out of range', $q$SELECT c.category, count(*) AS n FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'
   JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int > 6 THEN 'high' ELSE 'low' END) c ON true
   WHERE f.id = 20001 GROUP BY 1$q$);
-- a JSON null member is NULL in PostgreSQL 18 and an error before it (the text is not compared: it depends on the version)
INSERT INTO facts VALUES (20004, 4, '{"m_total": null}');
VACUUM (FREEZE, ANALYZE) facts;
SELECT label, same FROM check_query('json null', $q$SELECT count(*) AS n, sum(CAST(r.value AS float)) AS total FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total' WHERE f.id = 20004$q$);
-- the unit that holds the JSON null keeps the scan from the queries of the member before PostgreSQL 18 (its cast raises there where the scan reads NULL)
SELECT runs_in_scan($q$SELECT count(*) AS n, sum(CAST(r.value AS float)) AS total FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total'$q$)
       = (current_setting('server_version_num')::int >= 180000) AS json_null_scanned_from_18;
-- a document that is not an object raises PostgreSQL's error for the rows that reach the join, and so does a member that is no number: the scan is not used
INSERT INTO facts VALUES (20003, 3, '[1, 2]'), (20005, 5, '{"m_total": "abc"}');
VACUUM (FREEZE, ANALYZE) facts;
SELECT * FROM check_query('array document', $q$SELECT count(*), sum(CAST(r.value AS float)) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total' WHERE f.id = 20003$q$);
SELECT * FROM check_query('string member', $q$SELECT count(*), sum(CAST(r.value AS float)) FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'm_total' WHERE f.id = 20005$q$);
DROP FUNCTION check_query(text, text), outcome(text, bool), runs_in_scan(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
