-- EXISTS over the elements of an array that reads a column of a joined table (or a lookup per element) is a test of the joined tuples:
-- PostgreSQL cannot pull it up, the scan resolves each distinct element once against the lookup and keeps the rows with one that passes.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE entities (id int PRIMARY KEY, kind text);
INSERT INTO entities SELECT i, CASE WHEN i % 7 = 0 THEN NULL WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(0, 48) i;
CREATE TABLE mappings (kind text, category text, val text, mapped_category text, mapped_val text);
INSERT INTO mappings VALUES ('a', 'asp_l', '', 'asp_j', NULL), ('a', 'asp_l', '', 'duplicate', NULL), ('b', 'asp_l', '', 'asp_k', NULL),
 ('b', 'asp_a', 'x', 'asp_b', 'asp_i'), ('a', 'nul', NULL, 'never', NULL), ('a', 'asp_a', 'x', NULL, NULL), (NULL, 'asp_l', '', 'asp_j', NULL);
CREATE TABLE dates (day date PRIMARY KEY);
INSERT INTO dates SELECT d::date FROM generate_series('2026-01-01'::date, '2026-01-12', '1 day') d;
CREATE TABLE facts (id int, entity_id int, day date, document jsonb, metrics jsonb, payload text);
INSERT INTO facts
SELECT i, i % 52, '2026-01-01'::date + (i % 9),
 CASE WHEN i % 29 = 0 THEN NULL
      WHEN i % 31 = 0 THEN '{}'::jsonb
      ELSE jsonb_build_object('parts', CASE WHEN i % 7 = 0 THEN '[]'::jsonb
   WHEN i % 3 = 0 THEN '[{"category":"asp_l","val":"","sign":"positive","score":8}]'::jsonb
   ELSE '[{"category":"asp_l","val":"","sign":"positive","score":9},{"category":"asp_a","val":"x","sign":"negative","score":3},{"category":"zzz","val":"","sign":"neutral","score":null},{"category":"nul","val":null,"sign":"neutral","score":1}]'::jsonb END)
   || CASE WHEN i % 5 = 0 THEN '{"mapped":false}'::jsonb WHEN i % 11 = 0 THEN '{"mapped":null}'::jsonb WHEN i % 13 = 0 THEN '{"mapped":true}'::jsonb ELSE '{}'::jsonb END END,
 CASE WHEN i % 17 = 0 THEN '{}'::jsonb ELSE jsonb_build_object('p_total', i % 11, 'asp_j_asp_i', (i * 3) % 11) END,
 repeat('payload', 30)
FROM generate_series(1, 30000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, day, document, metrics, jsonb_exists(document, 'mapped'))
 WITH (paths = 'document.parts[]{category text,val text,score float8,sign text}, metrics.* float8');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dates;
ANALYZE entities;
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
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
-- The lookup-per-element shape: a lookup per element chosen by the entity's kind, the unmapped category of a document that is not mapped, the element's own otherwise.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n, avg(f.id) AS mean FROM facts f JOIN entities e ON e.id = f.entity_id
 WHERE EXISTS (SELECT 1 FROM LATERAL (
   SELECT (CASE WHEN jsonb_exists(f.document, 'mapped') THEN o.category ELSE m.mapped_category END) AS category
   FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   LEFT JOIN LATERAL (SELECT lm.mapped_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category AND lm.val = o.val
     AND lm.mapped_category IS NOT NULL AND NOT jsonb_exists(f.document, 'mapped') LIMIT 1) m ON true
   WHERE o.score IS NOT NULL) x WHERE x.category IN ('asp_j', 'asp_b'))
 GROUP BY e.kind
$q$);
-- Another element condition beside it, and the aggregates over the filtered rows.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n, count(DISTINCT f.id) AS items, sum(f.id) AS total FROM facts f JOIN entities e ON e.id = f.entity_id
 WHERE f.id > 100 AND EXISTS (SELECT 1 FROM LATERAL (
   SELECT (CASE WHEN jsonb_exists(f.document, 'mapped') THEN o.category ELSE m.mapped_category END) AS category, o.sign
   FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   LEFT JOIN LATERAL (SELECT lm.mapped_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category AND lm.val = o.val
     AND lm.mapped_category IS NOT NULL AND NOT jsonb_exists(f.document, 'mapped') LIMIT 1) m ON true
   WHERE o.score IS NOT NULL) x WHERE x.category IN ('asp_j') AND x.sign IN ('positive', 'neutral'))
 GROUP BY e.kind
$q$);
-- The element is compared with a column of the joined table, without a lookup.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n FROM facts f JOIN entities e ON e.id = f.entity_id
 WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   WHERE o.score IS NOT NULL AND o.category = CASE e.kind WHEN 'a' THEN 'asp_l' ELSE 'asp_a' END)
 GROUP BY e.kind
$q$);
-- Two of them, and a count of the elements of another array of the same rows (the tuples repeat the row).
SELECT * FROM check_query($q$
 SELECT r.key, count(*) AS n, avg(r.value::float8) AS mean FROM facts f JOIN entities e ON e.id = f.entity_id CROSS JOIN LATERAL jsonb_each(f.metrics) r
 WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   WHERE o.category = CASE e.kind WHEN 'a' THEN 'asp_l' ELSE 'asp_a' END)
   AND EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   WHERE o.sign = CASE e.kind WHEN 'a' THEN 'positive' ELSE 'negative' END)
 GROUP BY r.key
$q$);
-- The entities with no row, the ones with a NULL kind and the documents with no array: the same rows as PostgreSQL's.
SELECT * FROM check_query($q$
 SELECT e.id IS NULL AS no_entity, e.kind, count(*) AS n FROM facts f LEFT JOIN entities e ON e.id = f.entity_id
 WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   WHERE o.category = 'asp_l' AND (e.kind IS NULL OR o.sign <> 'negative'))
 GROUP BY 1, 2
$q$);
-- The test in the nullable input of a preserved side: the buckets that no row reaches stay.
SELECT * FROM check_query($q$
 SELECT d.day, count(DISTINCT f.id) AS n, avg(f.id) AS mean FROM dates d
 LEFT JOIN (SELECT f.id, f.day FROM facts f JOIN entities e ON e.id = f.entity_id
   WHERE f.id % 4 <> 3 AND EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
     WHERE o.category = CASE e.kind WHEN 'a' THEN 'asp_l' ELSE 'asp_a' END)) f ON f.day = d.day
 GROUP BY d.day
$q$);
-- NOT EXISTS, and an EXISTS inside an aggregate's FILTER, stay with PostgreSQL.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n FROM facts f JOIN entities e ON e.id = f.entity_id
 WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   WHERE o.category = CASE e.kind WHEN 'a' THEN 'asp_l' ELSE 'asp_a' END)
 GROUP BY e.kind
$q$);
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
   WHERE o.category = CASE e.kind WHEN 'a' THEN 'asp_l' ELSE 'asp_a' END)) FROM facts f JOIN entities e ON e.id = f.entity_id
 GROUP BY e.kind
$q$);
-- EXPLAIN: the test, the lookup it resolves per element, and what the scan runs.
EXPLAIN (COSTS OFF)
SELECT e.kind, count(*) FROM facts f JOIN entities e ON e.id = f.entity_id
WHERE EXISTS (SELECT 1 FROM LATERAL (
  SELECT (CASE WHEN jsonb_exists(f.document, 'mapped') THEN o.category ELSE m.mapped_category END) AS category
  FROM jsonb_to_recordset(f.document -> 'parts') AS o(category text, val text, score float8, sign text)
  LEFT JOIN LATERAL (SELECT lm.mapped_category FROM mappings lm WHERE lm.kind = e.kind AND lm.category = o.category AND lm.val = o.val
    AND lm.mapped_category IS NOT NULL AND NOT jsonb_exists(f.document, 'mapped') LIMIT 1) m ON true
  WHERE o.score IS NOT NULL) x WHERE x.category IN ('asp_j', 'asp_b'))
GROUP BY e.kind;
-- Errors come from PostgreSQL's evaluation, for the rows and elements it reaches: an expression that may raise keeps the normal plan, and the
-- row that raises is the same in both. The element that raises is after the one that passes in the rows below, which EXISTS never evaluates.
CREATE TABLE raising (id int, entity_id int, document jsonb);
INSERT INTO raising
SELECT i, i % 52, jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('category', 'asp_l', 'val', '1'),
  jsonb_build_object('category', 'asp_a', 'val', CASE WHEN i % 1000 = 0 THEN 'x' ELSE '2' END)))
FROM generate_series(1, 20000) i;
CREATE INDEX raising_col ON raising USING sift_col (id, entity_id, document) WITH (paths = 'document.parts[]{category text,val text}');
VACUUM (FREEZE, ANALYZE) raising;
CREATE FUNCTION check_error(q text) RETURNS TABLE (pushed bool, same bool, message text) LANGUAGE plpgsql AS $$
DECLARE on_msg text; off_msg text; line text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  BEGIN EXECUTE q; off_msg := 'no error'; EXCEPTION WHEN OTHERS THEN off_msg := SQLERRM; END;
  SET LOCAL sift.enable_agg = on;
  BEGIN EXECUTE q; on_msg := 'no error'; EXCEPTION WHEN OTHERS THEN on_msg := SQLERRM; END;
  pushed := false;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP pushed := pushed OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  same := on_msg = off_msg;
  message := on_msg;
  RETURN NEXT;
END $$;
-- the cast reaches the element that raises (no element of the row passes before it)
SELECT * FROM check_error($q$
 SELECT e.kind, count(*) FROM raising r JOIN entities e ON e.id = r.entity_id
 WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(r.document -> 'parts') AS o(category text, val text)
   WHERE o.val::int > 100 AND o.category <> e.kind) GROUP BY e.kind
$q$);
-- the first element passes: the second, which raises, is not evaluated
SELECT * FROM check_error($q$
 SELECT e.kind, count(*) FROM raising r JOIN entities e ON e.id = r.entity_id
 WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(r.document -> 'parts') AS o(category text, val text)
   WHERE (o.category = 'asp_l' OR o.val::int > 0) AND e.kind IS DISTINCT FROM 'zz') GROUP BY e.kind
$q$);
-- the row that raises is dropped by another condition before the test
SELECT * FROM check_error($q$
 SELECT e.kind, count(*) FROM raising r JOIN entities e ON e.id = r.entity_id
 WHERE r.id % 1000 <> 0 AND EXISTS (SELECT 1 FROM jsonb_to_recordset(r.document -> 'parts') AS o(category text, val text)
   WHERE o.val::int > 100 AND o.category <> e.kind) GROUP BY e.kind
$q$);
DROP FUNCTION check_query(text), check_error(text);
DROP TABLE facts, raising, entities, mappings, dates;
DROP EXTENSION pg_sift;
