-- Conditions of the nullable input of an outer join (a calendar LEFT JOIN a derived table) that are tests of the rows' arrays: an EXISTS
-- over the elements, written with a sub-select that PostgreSQL pulls up, a jsonpath test, and the aggregate filters that read them.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dates (day date PRIMARY KEY);
INSERT INTO dates SELECT d::date FROM generate_series('2026-01-01'::date, '2026-01-20', '1 day') d;
CREATE TABLE entities (id int PRIMARY KEY, kind text);
INSERT INTO entities SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(1, 50) i;
CREATE TABLE facts (id int, day date, entity_id int, deleted_at timestamp, doc jsonb, notes jsonb);
INSERT INTO facts
SELECT i, '2026-01-01'::date + (i % 15), 1 + i % 60, CASE WHEN i % 17 = 0 THEN now() END,
       CASE WHEN i % 19 = 0 THEN NULL WHEN i % 13 = 0 THEN '{"items": []}'::jsonb WHEN i % 29 = 0 THEN '{}'::jsonb
            ELSE jsonb_build_object('items', jsonb_build_array(
                   jsonb_build_object('name', 'x', 'tag', (ARRAY['hot', 'cold', 'dry'])[1 + i % 3], 'score', CASE WHEN i % 7 = 0 THEN NULL ELSE i % 10 END),
                   jsonb_build_object('name', 'y', 'tag', (ARRAY['hot', 'cold', 'dry'])[1 + (i / 3) % 3], 'score', i % 5))) END,
       CASE WHEN i % 5 = 0 THEN NULL WHEN i % 5 = 1 THEN '[]'::jsonb
            WHEN i % 5 = 2 THEN '[{"type": "staff"}]'::jsonb ELSE '[{"type": "guest"}, {"type": "staff"}]'::jsonb END
FROM generate_series(1, 6000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, day, entity_id, deleted_at, doc, notes)
  WITH (paths = 'doc.items[]{name text, tag text, score float8}, notes[]{type text}');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dates;
ANALYZE entities;
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
-- An EXISTS over the elements that PostgreSQL pulls up from a sub-select of its own (the sub-plan keeps the function with the dead entry of the sub-select before it).
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n
 FROM facts f JOIN entities e ON e.id = f.entity_id
 WHERE EXISTS (SELECT * FROM LATERAL (SELECT tag AS t, score FROM jsonb_to_recordset(f.doc -> 'items') AS o(name text, tag text, score float8) WHERE o.score IS NOT NULL) o WHERE o.t IN ('hot'))
 GROUP BY 1
$q$);
-- The same test as the condition of the derived table of a calendar join, with a jsonpath test of its rows computed inside it and read by aggregate filters.
SELECT * FROM check_query($q$
 SELECT d.day, count(DISTINCT f.id) AS n, count(DISTINCT f.id) FILTER (WHERE f.staff IS TRUE) AS staff
 FROM dates d LEFT JOIN (
   SELECT f.id, f.day, jsonb_path_exists(f.notes, '$[*] ? (@.type == "staff")') AS staff
   FROM facts f JOIN entities e ON e.id = f.entity_id
   WHERE f.deleted_at IS NULL
     AND EXISTS (SELECT * FROM LATERAL (SELECT tag AS t, score FROM jsonb_to_recordset(f.doc -> 'items') AS o(name text, tag text, score float8) WHERE o.score IS NOT NULL) o WHERE o.t IN ('hot'))
 ) f ON f.day = d.day
 GROUP BY 1
$q$);
-- A jsonpath test as the condition of the derived table and as the filter of an aggregate, over days that have no row.
SELECT * FROM check_query($q$
 SELECT d.day, count(DISTINCT f.id) AS n, count(DISTINCT f.id) FILTER (WHERE f.guest IS TRUE) AS guest
 FROM dates d LEFT JOIN (
   SELECT f.id, f.day, jsonb_path_exists(f.notes, '$[*] ? (@.type == "guest")') AS guest
   FROM facts f
   WHERE f.deleted_at IS NULL AND jsonb_path_exists(f.notes, '$[*] ? (@.type == "staff")')
 ) f ON f.day = d.day
 GROUP BY 1
$q$);
-- A computed test that is true for an absent input stays with PostgreSQL: it would be true for the calendar's empty days.
SELECT * FROM check_query($q$
 SELECT d.day, count(*) FILTER (WHERE f.none IS TRUE) AS n
 FROM dates d LEFT JOIN (SELECT f.id, f.day, f.notes IS NULL AS none FROM facts f) f ON f.day = d.day
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE facts, entities, dates;
DROP EXTENSION pg_sift;
