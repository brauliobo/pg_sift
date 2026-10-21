-- The member `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` picks is the one the key equals: a non-deterministic collation equates k and K, so the join
-- matches both and the query keeps its plan. Only whether the results agree is printed: the pushdown needs PostgreSQL 18.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE TABLE facts (id int, entity_id int, metrics jsonb);
INSERT INTO facts
SELECT i, i % 40, CASE i % 4 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN jsonb_build_object('k', i % 10, 'K', i % 3) ELSE jsonb_build_object('k', i % 7) END
FROM generate_series(1, 4000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, metrics) WITH (paths = 'metrics.* float8');
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
 SELECT f.entity_id % 3 AS g, count(*) AS n, count(CAST(r.value AS float)) AS members, sum(CAST(r.value AS float)) AS total
 FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'k' GROUP BY 1
$q$) AS key;
SELECT same($q$
 SELECT f.entity_id % 3 AS g, count(*) AS n, count(CAST(r.value AS float)) AS members, sum(CAST(r.value AS float)) AS total
 FROM facts f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key = 'k' COLLATE ci GROUP BY 1
$q$) AS insensitive_key;
DROP FUNCTION same(text), outcome(text, bool);
DROP TABLE facts;
DROP COLLATION ci;
DROP EXTENSION pg_sift;
