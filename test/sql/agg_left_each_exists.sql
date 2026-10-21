-- The member of `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` belongs to the query's own FROM list: an EXISTS over the members of another document
-- is compiled on its own (the scan used to resolve its elements as that member and crash). The pushdown needs PostgreSQL 18.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, entity_id int, metrics jsonb, extra jsonb);
INSERT INTO facts
SELECT i, i % 40, jsonb_build_object('k', i % 7), jsonb_build_object('a', i % 9, 'b', i % 5)
FROM generate_series(1, 4000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, metrics, extra) WITH (paths = 'metrics.* float8, extra.* float8');
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
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT outcome(q, false) = outcome(q, true) $$;
SELECT same($q$
 SELECT f.entity_id % 3 AS g, count(*) AS n, sum(CAST(r.value AS float)) AS total
 FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'k'
 WHERE EXISTS (SELECT 1 FROM jsonb_each(f.extra) e WHERE (e.value)::float8 > 3) GROUP BY 1
$q$) AS exists_members;
DROP FUNCTION same(text), outcome(text, bool);
DROP TABLE facts;
DROP EXTENSION pg_sift;
