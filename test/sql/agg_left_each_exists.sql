-- The member of `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` belongs to the query's own FROM list: an EXISTS over the members of another document
-- is compiled on its own (the scan used to resolve its elements as that member and crash).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, entity_id int, metrics jsonb, extra jsonb);
INSERT INTO facts
SELECT i, i % 40, jsonb_build_object('k', i % 7), jsonb_build_object('a', i % 9, 'b', i % 5)
FROM generate_series(1, 4000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, metrics, extra) WITH (paths = 'metrics.* float8, extra.* float8');
VACUUM (FREEZE, ANALYZE) facts;
\ir test/agg_helpers.sql
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT outcome(q, false) = outcome(q, true) $$;
SELECT same($q$
 SELECT f.entity_id % 3 AS g, count(*) AS n, sum(CAST(r.value AS float)) AS total
 FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'k'
 WHERE EXISTS (SELECT 1 FROM jsonb_each(f.extra) e WHERE (e.value)::float8 > 3) GROUP BY 1
$q$) AS exists_members;
DROP FUNCTION same(text), outcome(text, bool), runs_in_scan(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
