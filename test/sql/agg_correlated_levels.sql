-- A lateral child, or a sub-select the scan asks once, that reads a column of a query above the aggregate's own stays on the normal plan: the scan would
-- never see the parameter.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, kind text, payload text);
INSERT INTO facts SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END, repeat('payload', 50) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, kind);
CREATE TABLE lookup_values (kind text, region text, label text);
INSERT INTO lookup_values VALUES ('a', 'east', 'ae'), ('b', 'east', 'be'), ('a', 'west', 'aw'), ('b', 'west', 'bw');
CREATE TABLE region_kinds (kind text, region text);
INSERT INTO region_kinds VALUES ('a', 'east'), ('b', 'west'), ('b', 'east');
CREATE TABLE regions (id int, region text);
INSERT INTO regions VALUES (1, 'east'), (2, 'west'), (3, 'north');
CREATE TABLE plain_facts AS SELECT id, kind, payload FROM facts;
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE plain_facts;
ANALYZE lookup_values;
ANALYZE regions;
ANALYZE region_kinds;
CREATE FUNCTION run_query(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  PERFORM set_config('sift.enable_agg', agg::text, true);
  EXECUTE 'SELECT coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''') FROM (' || q || ') t' INTO r;
  RETURN r;
EXCEPTION WHEN OTHERS THEN RETURN 'ERROR: ' || SQLERRM;
END $$;
CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, same bool, result text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  result := run_query(q, false);
  same := result = run_query(q, true);
  pushed := false;
  PERFORM set_config('sift.enable_agg', 'on', true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
  END LOOP;
  RETURN NEXT;
END $$;
-- The child reads the grandparent's row: every outer row has its own answer.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FILTER (WHERE l.label = 'ae')
   FROM facts f LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind = f.kind AND m.region = r.region LIMIT 1) l ON true)
 FROM regions r
$q$);
-- Not lateral: a sub-select of the FROM list reads the outer query too.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FILTER (WHERE l.label = 'ae')
   FROM facts f LEFT JOIN (SELECT kind, label FROM lookup_values m WHERE m.region = r.region) l ON l.kind = f.kind)
 FROM regions r
$q$);
-- A function that reads the grandparent's column.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FROM facts f CROSS JOIN LATERAL generate_series(1, r.id) g(i) WHERE g.i = 1 + f.id % 4)
 FROM regions r
$q$);
-- The aggregate of the sub-select reads the outer column itself: a plan parameter the scan rejects already.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FROM facts f LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind = f.kind LIMIT 1) l ON true
   WHERE l.label = 'ae' AND r.region = 'east')
 FROM regions r
$q$);
-- A hashed NOT IN of the aggregate's WHERE asks its sub-select once; the sub-select reads the outer row through a CTE or a sub-select of its own, which
-- PostgreSQL plans from a copy.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FROM facts f
   WHERE f.kind NOT IN (WITH c AS MATERIALIZED (SELECT kind FROM region_kinds k WHERE k.region = r.region) SELECT kind FROM c))
 FROM regions r
$q$);
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FROM facts f
   WHERE f.kind NOT IN (SELECT s.kind FROM (SELECT kind FROM region_kinds k WHERE k.region = r.region LIMIT 10) s))
 FROM regions r
$q$);
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FROM facts f
   WHERE f.kind NOT IN (SELECT kind FROM region_kinds k WHERE k.region = r.region))
 FROM regions r
$q$);
-- Control: the same tests of a sub-select that reads no outer row are asked once, and the outer rows replay the groups.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FROM facts f
   WHERE f.kind NOT IN (WITH c AS MATERIALIZED (SELECT kind FROM region_kinds k WHERE k.region = 'west') SELECT kind FROM c))
 FROM regions r
$q$);
-- Control: a child that reads the sub-select's own tables only is still joined by the scan, and the outer rows replay its groups.
SELECT * FROM check_query($q$
 SELECT r.id, (SELECT count(*) FILTER (WHERE l.label = 'ae')
   FROM facts f LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind = f.kind AND m.region = 'east' LIMIT 1) l ON true)
 FROM regions r
$q$);
SELECT * FROM check_query($q$
 SELECT count(*) FILTER (WHERE l.label = 'ae') FROM facts f
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind = f.kind AND m.region = 'east' LIMIT 1) l ON true
$q$);
-- A table with no column index is not scanned: the sub-selects of its query are planned by PostgreSQL only.
SELECT * FROM check_query($q$
 SELECT count(*) FILTER (WHERE l.label = 'ae') FROM plain_facts f
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind = f.kind AND m.region = 'east' LIMIT 1) l ON true
$q$);
DROP FUNCTION check_query(text);
DROP FUNCTION run_query(text, bool);
DROP TABLE facts, plain_facts, lookup_values, region_kinds, regions;
DROP EXTENSION pg_sift;
