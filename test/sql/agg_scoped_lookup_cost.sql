-- The lookup per element of an EXISTS over an array (a LEFT JOIN LATERAL of one mapped row per element) is a child of the scan whose range table index
-- is in the EXISTS' own tables, not in the query's: the cost of the scan prices it without looking that index up in the query's range table (it read
-- past the array of the planner's tables, a crash on a build with sanitizers). Each shape answers as the normal plan does, with the scan chosen.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, kind text);
INSERT INTO dim_a SELECT i, CASE WHEN i % 7 = 0 THEN NULL WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(0, 48) i;
CREATE TABLE mappings (kind text, category text, derived_category text);
INSERT INTO mappings VALUES ('a', 'catg_l', 'catg_j'), ('b', 'catg_l', 'catg_k'), ('b', 'catg_a', 'catg_b'), ('a', 'catg_a', NULL), (NULL, 'catg_l', 'catg_j');
CREATE TABLE facts (id int, dim_a_id int, document jsonb);
INSERT INTO facts
SELECT i, i % 52, CASE WHEN i % 29 = 0 THEN NULL
  WHEN i % 3 = 0 THEN '{"parts":[{"category":"catg_l","score":8}]}'::jsonb
  ELSE '{"parts":[{"category":"catg_l","score":9},{"category":"catg_a","score":3},{"category":"zzz","score":null}]}'::jsonb END
FROM generate_series(1, 6000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, document) WITH (paths = 'document.parts[]{category text,score float8}');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dim_a;
ANALYZE mappings;
\ir test/agg_check_query.sql
SELECT s.name, c.* FROM (VALUES
 ('grouped by the entity', $q$
  SELECT e.kind, count(*) AS n, avg(f.id) AS mean FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
  WHERE EXISTS (SELECT 1 FROM LATERAL (SELECT coalesce(m.derived_category, o.category) AS category
    FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, score float8)
    LEFT JOIN LATERAL (SELECT lm.derived_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category
      AND lm.derived_category IS NOT NULL LIMIT 1) m ON true
    WHERE o.score IS NOT NULL) x WHERE x.category IN ('catg_j', 'catg_b'))
  GROUP BY e.kind $q$),
 ('without groups', $q$
  SELECT count(*) AS n, sum(f.id) AS total FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
  WHERE EXISTS (SELECT 1 FROM LATERAL (SELECT coalesce(m.derived_category, o.category) AS category
    FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, score float8)
    LEFT JOIN LATERAL (SELECT lm.derived_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category
      AND lm.derived_category IS NOT NULL LIMIT 1) m ON true) x WHERE x.category = 'catg_k') $q$),
 ('a range on the ids and distinct ids', $q$
  SELECT e.kind, count(DISTINCT f.id) AS n FROM facts f JOIN dim_a e ON e.id = f.dim_a_id
  WHERE f.id > 100 AND EXISTS (SELECT 1 FROM LATERAL (SELECT coalesce(m.derived_category, o.category) AS category
    FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, score float8)
    LEFT JOIN LATERAL (SELECT lm.derived_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category
      AND lm.derived_category IS NOT NULL LIMIT 1) m ON true) x WHERE x.category IN ('zzz', 'catg_j'))
  GROUP BY e.kind $q$)
) s(name, q), LATERAL check_query(s.q) c;
DROP FUNCTION check_query(text);
DROP TABLE facts, dim_a, mappings;
DROP EXTENSION pg_sift;
