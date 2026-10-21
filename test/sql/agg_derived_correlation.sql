-- A correlated LIMIT child that reads a document only through a non-raising call (key presence) takes the call's value as its parameter.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, kind text);
INSERT INTO dim_a SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(0, 49) i;
CREATE TABLE mappings (kind text, category text, val text, derived_category text, derived_val text);
INSERT INTO mappings VALUES ('a', 'catg_l', '', 'catg_j', NULL), ('a', 'catg_l', '', 'duplicate', NULL), ('b', 'catg_l', '', 'catg_k', NULL),
 ('b', 'catg_a', 'x', 'catg_b', 'catg_i'), ('a', 'nul', NULL, 'never', NULL);
CREATE TABLE facts (id int, dim_a_id int, kind text, document jsonb, payload text);
INSERT INTO facts
SELECT i, i % 50, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END,
 jsonb_build_object('parts', CASE WHEN i % 7 = 0 THEN '[]'::jsonb
   WHEN i % 3 = 0 THEN '[{"category":"catg_l","val":"","dir_a":"up","score":8}]'::jsonb
   ELSE '[{"category":"catg_l","val":"","dir_a":"up","score":9},{"category":"catg_a","val":"x","dir_a":"down","score":3},{"category":"zzz","val":"","dir_a":"flat","score":null}]'::jsonb END)
  || CASE WHEN i % 5 = 0 THEN '{"flag_a":false}'::jsonb WHEN i % 11 = 0 THEN '{"flag_a":null}'::jsonb ELSE '{}'::jsonb END,
 repeat('payload', 50)
FROM generate_series(1, 60000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, kind, document, jsonb_exists(document, 'flag_a'))
 WITH (paths = 'document.parts[]{category text,val text,score float8,dir_a text}');
-- Documents whose root is not an object: key presence on them is the PostgreSQL rule for arrays and scalars.
CREATE TABLE odd_facts (id int, kind text, document jsonb, payload text);
INSERT INTO odd_facts
SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END,
 CASE i % 8 WHEN 0 THEN NULL WHEN 1 THEN '[]' WHEN 2 THEN '"flag_a"' WHEN 3 THEN '["flag_a"]' WHEN 4 THEN 'null' WHEN 5 THEN '{"flag_a":true}' WHEN 6 THEN '["x"]' ELSE '{}' END::jsonb,
 repeat('payload', 50)
FROM generate_series(1, 40000) i;
CREATE INDEX odd_facts_col ON odd_facts USING sift_col (id, kind, document, jsonb_exists(document, 'flag_a')) WITH (paths = 'document.flag_a astext');
-- The same table without the indexed call: the child needs the whole document, which the index does not hold.
CREATE TABLE plain_facts AS SELECT id, kind, document, payload FROM facts;
CREATE INDEX plain_facts_col ON plain_facts USING sift_col (id, kind, document)
 WITH (paths = 'document.parts[]{category text,val text,score float8,dir_a text}');
-- The operator forms (? and ->>) and the type of the whole document are derived calls too; some documents are arrays.
CREATE TABLE ops_facts AS
SELECT id, dim_a_id, kind,
 CASE WHEN id % 4 = 0 THEN document || '{"tag":"catg_l"}' WHEN id % 4 = 1 THEN document || '{"tag":"catg_a"}' WHEN id % 13 = 2 THEN '[1]'::jsonb ELSE document END AS document, payload
FROM facts;
CREATE INDEX ops_facts_col ON ops_facts USING sift_col (id, dim_a_id, kind, document, (document ? 'flag_a'), (document ->> 'tag'), jsonb_typeof(document))
 WITH (paths = 'document.parts[]{category text}');
VACUUM (FREEZE, ANALYZE) ops_facts;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (FREEZE, ANALYZE) odd_facts;
VACUUM (FREEZE, ANALYZE) plain_facts;
ANALYZE dim_a;
ANALYZE mappings;
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
  -- before PostgreSQL 18 each disabled scan adds its cost to the child plans of every distinct input, so the plan with the scans off does not tell: the cheapest plan has to be the scan
  eligible := eligible OR (pushed AND current_setting('server_version_num')::int < 180000);
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
-- The child's condition reads key presence only: false, null and missing keys.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = 'a' AND NOT jsonb_exists(f.document, 'flag_a') LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- Presence on documents that are arrays, strings, JSON null and SQL NULL follows PostgreSQL's rule, per document.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM odd_facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = f.kind AND NOT jsonb_exists(f.document, 'flag_a') LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- Another outer column in the same child, and duplicate candidates broken by the child's own ORDER BY.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = f.kind AND NOT jsonb_exists(f.document, 'flag_a')
   ORDER BY derived_category DESC LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- Mapped parts per item: the presence also chooses the category in the lateral's output, and the unmapped parts drop out.
SELECT * FROM check_query($q$
 SELECT f.id, mo.category, mo.val, avg(mo.score) AS score, count(f.id) FILTER (WHERE mo.dir_a = 'up') AS positive
 FROM facts f INNER JOIN LATERAL (
  SELECT (CASE WHEN jsonb_exists(f.document, 'flag_a') THEN o.source_category ELSE rm.derived_category END) AS category,
         (CASE WHEN jsonb_exists(f.document, 'flag_a') THEN o.source_val ELSE rm.derived_val END) AS val, o.dir_a, o.score
  FROM LATERAL (SELECT category AS source_category, val AS source_val, dir_a, score
    FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, dir_a text, score float8) WHERE o.score IS NOT NULL) o
  LEFT JOIN LATERAL (SELECT derived_category, derived_val FROM mappings m WHERE m.derived_category IS NOT NULL AND m.kind = f.kind
    AND m.category = o.source_category AND m.val = o.source_val AND NOT jsonb_exists(f.document, 'flag_a') LIMIT 1) rm ON true
 ) mo ON true
 WHERE mo.category IS NOT NULL GROUP BY f.id, mo.category, mo.val
$q$);
-- The mean of the per-item means over the same mapping (the items weigh equally whatever their part count).
SELECT * FROM check_query($q$
 SELECT category, avg(score) AS score, count(*) AS items FROM (
  SELECT f.id, mo.category, avg(mo.score) AS score
  FROM facts f INNER JOIN LATERAL (
   SELECT (CASE WHEN jsonb_exists(f.document, 'flag_a') THEN o.source_category ELSE rm.derived_category END) AS category, o.score
   FROM LATERAL (SELECT category AS source_category, val AS source_val, score
     FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8) WHERE o.score IS NOT NULL) o
   LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = f.kind AND m.category = o.source_category
     AND m.val = o.source_val AND NOT jsonb_exists(f.document, 'flag_a') LIMIT 1) rm ON true
  ) mo ON true WHERE mo.category IS NOT NULL GROUP BY f.id, mo.category) per_item
 GROUP BY category
$q$);
-- Stays native: the child reads the document in another way too, so it needs the whole document.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = 'a' AND NOT jsonb_exists(f.document, 'flag_a')
   AND f.document -> 'parts' IS NOT NULL LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- Stays native: the index holds no presence column, and a presence test of a whole object has no other leaf.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM plain_facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = 'a' AND NOT jsonb_exists(f.document, 'flag_a') LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- The operator ? is the call jsonb_exists, as the index column spells it.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM ops_facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = f.kind AND NOT (f.document ? 'flag_a') LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- ->> of a key: a NULL for the documents that are arrays or lack the key.
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM ops_facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = f.kind AND m.category = (f.document ->> 'tag') LIMIT 1) m ON true
 GROUP BY 1
$q$);
-- The type of the whole document (one argument).
SELECT * FROM check_query($q$
 SELECT m.derived_category, count(*) AS n FROM ops_facts f
 LEFT JOIN LATERAL (SELECT derived_category FROM mappings m WHERE m.kind = f.kind AND jsonb_typeof(f.document) = 'object' AND m.category = 'catg_l' LIMIT 1) m ON true
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, odd_facts, plain_facts, ops_facts, dim_a, mappings;
DROP EXTENSION pg_sift;
