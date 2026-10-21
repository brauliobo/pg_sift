-- A lateral child that reads a column of a query above the aggregate's own stays on the normal plan: the scan would never see the parameter.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, kind text, payload text);
INSERT INTO facts SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END, repeat('payload', 50) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, kind);
CREATE TABLE lookup_values (kind text, region text, label text);
INSERT INTO lookup_values VALUES ('a', 'east', 'ae'), ('b', 'east', 'be'), ('a', 'west', 'aw'), ('b', 'west', 'bw');
CREATE TABLE regions (id int, region text);
INSERT INTO regions VALUES (1, 'east'), (2, 'west'), (3, 'north');
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE lookup_values;
ANALYZE regions;
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
