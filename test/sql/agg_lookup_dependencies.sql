-- A lookup can read columns of earlier lookups, including NULL-extended rows.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, entity_id int, amount int, payload text);
INSERT INTO events SELECT i, i % 100, i % 11, repeat('payload', 50) FROM generate_series(1, 50000) i;
CREATE INDEX events_col ON events USING sift_col (id, entity_id, amount);
CREATE TABLE entities (id int, code text, kind text);
INSERT INTO entities SELECT i, CASE WHEN i % 9 = 0 THEN NULL ELSE 'c' || i % 5 END, 'k' || i % 3 FROM generate_series(1, 95) i;
INSERT INTO entities VALUES (1, 'c1', 'duplicate');
CREATE TABLE labels (code text, tag text);
INSERT INTO labels VALUES ('c0', 'zero'), ('c1', 'one'), ('c1', 'duplicate'), ('c2', NULL), (NULL, 'null');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE entities; ANALYZE labels;
CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM
    ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
SELECT * FROM check_query($q$
 SELECT l.tag, count(*) AS n, sum(f.amount) AS total
 FROM events f JOIN entities e ON e.id = f.entity_id JOIN labels l ON l.code = e.code
 GROUP BY l.tag
$q$);
SELECT * FROM check_query($q$
 SELECT coalesce(l.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f LEFT JOIN entities e ON e.id = f.entity_id LEFT JOIN labels l ON l.code = e.code
 GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) AS n
 FROM events f JOIN entities e ON e.id = f.entity_id JOIN labels l ON l.code = e.code
 WHERE l.tag <> e.kind GROUP BY e.kind
$q$);
-- The deduplicated membership CTE stays materialized and is evaluated by PostgreSQL.
SELECT * FROM check_query($q$
 WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM entities WHERE id < 90)
 SELECT coalesce(l.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f JOIN ids p ON p.id = f.entity_id
 JOIN entities e ON e.id = p.id LEFT JOIN labels l ON l.code = e.code
 GROUP BY 1
$q$);
-- Reusing a materialized CTE must share the same statement result.
SELECT * FROM check_query($q$
 WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM entities WHERE id < 90)
 SELECT count(*) AS n FROM events f JOIN ids p ON p.id = f.entity_id
 JOIN ids q ON q.id = p.id
$q$);
-- Three lookup stages retain duplicate fanout and NULL extensions from earlier stages.
SELECT * FROM check_query($q$
 SELECT coalesce(b.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids,
        count(a.tag) AS labelled
 FROM events f LEFT JOIN entities e ON e.id = f.entity_id
 LEFT JOIN labels a ON a.code = e.code
 LEFT JOIN labels b ON b.tag = a.tag AND b.code = e.code
 GROUP BY GROUPING SETS ((coalesce(b.tag, e.kind)), ())
$q$);
-- The grouping query can reference its statement's materialized CTE from a nested query level.
SELECT * FROM check_query($q$
 WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM entities WHERE id < 90)
 SELECT sum(n) AS n FROM (
   SELECT e.kind, count(*) AS n FROM events f JOIN ids p ON p.id = f.entity_id
   JOIN entities e ON e.id = p.id GROUP BY e.kind
 ) grouped
$q$);
-- RTE order is not dependency order: the entity lookup supplies the earlier membership lookup's key.
SELECT * FROM check_query($q$
 WITH ids AS MATERIALIZED (SELECT DISTINCT id FROM entities WHERE id < 90)
 SELECT coalesce(l.tag, e.kind) AS label, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f JOIN (ids p JOIN entities e ON e.id = p.id) ON e.id = f.entity_id
 LEFT JOIN labels l ON l.code = e.code GROUP BY 1
$q$);
-- FILTER uses joined tuple identity: different matches of one fact may pass different predicates.
SELECT * FROM check_query($q$
 SELECT e.kind, count(*) FILTER (WHERE l.tag = 'one' OR f.amount > 7) AS n,
        count(DISTINCT f.id) FILTER (WHERE l.tag IS NULL) AS missing,
        sum(f.amount) FILTER (WHERE l.tag <> e.kind) AS total
 FROM events f LEFT JOIN entities e ON e.id = f.entity_id
 LEFT JOIN labels l ON l.code = e.code GROUP BY GROUPING SETS ((e.kind), ())
$q$);
-- A FILTER can raise only after every join has selected the aggregate's input.
SELECT * FROM check_query($q$
 SELECT count(*) FILTER (WHERE 10 / (length(l.tag) - 4) > 0) AS n
 FROM events f JOIN entities e ON e.id = f.entity_id
 LEFT JOIN labels l ON l.code = e.code
 JOIN labels k ON k.tag = l.tag AND k.tag = 'one'
$q$);
-- The same expression must still raise when an eligible joined tuple reaches it.
DO $$
DECLARE mode text;
BEGIN
  FOREACH mode IN ARRAY ARRAY['off', 'on'] LOOP
    PERFORM set_config('sift.enable_agg', mode, true);
    BEGIN
      EXECUTE $q$SELECT count(*) FILTER (WHERE 10 / (length(l.tag) - 4) > 0)
        FROM events f JOIN entities e ON e.id = f.entity_id
        LEFT JOIN labels l ON l.code = e.code$q$;
      RAISE EXCEPTION 'expected division_by_zero with aggregation %', mode;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
  END LOOP;
END $$;
-- Real executor parameters are not local nullable-input markers, even when their IDs coincide.
SELECT * FROM check_query($q$
 SELECT (SELECT 0) AS p0, (SELECT 1) AS p1, (SELECT 2) AS p2,
        (SELECT 3) AS p3, (SELECT 4) AS p4, (SELECT 5) AS p5,
        (SELECT 6) AS p6, (SELECT 7) AS p7, (SELECT 8) AS p8,
        CASE WHEN (SELECT true) THEN f.id % 3 ELSE f.id % 3 + 10 END AS key, count(*) AS n
 FROM events f GROUP BY 10
$q$);
-- Exercise additional parameter IDs as expression input capacity grows.
DO $$
DECLARE q text; mismatch bigint;
BEGIN
  SELECT 'SELECT ' || string_agg('(SELECT ' || i || ') AS p' || i, ', ') ||
    ', CASE WHEN (SELECT true) THEN f.id % 3 ELSE f.id % 3 + 10 END AS key, count(*) AS n' ||
    ' FROM events f GROUP BY 15' INTO q FROM generate_series(0, 13) i;
  SELECT differences INTO mismatch FROM check_query(q);
  IF mismatch <> 0 THEN RAISE EXCEPTION 'executor parameter changed grouping results'; END IF;
END $$;
DROP FUNCTION check_query(text);
DROP TABLE events, entities, labels;
DROP EXTENSION pg_sift;
