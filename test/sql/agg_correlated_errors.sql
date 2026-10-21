-- A lateral child runs for the rows that reach it, in the order PostgreSQL's plan chooses: the scan joins one that cannot raise, and one that can keeps the
-- normal plan, so no statement raises under the scan and not in the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, entity_id int, code text, payload text);
INSERT INTO facts SELECT i, i % 40, CASE WHEN i % 40 = 0 THEN 'none' ELSE (i % 9)::text END, repeat('payload', 50) FROM generate_series(1, 40000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, code);
CREATE TABLE dims (id int, ok bool);
INSERT INTO dims SELECT i, i % 3 <> 0 FROM generate_series(1, 39) i;
CREATE TABLE tags (id int, entity_id int);
INSERT INTO tags SELECT i, i FROM generate_series(1, 39) i;
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dims;
ANALYZE tags;
CREATE FUNCTION run_query(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  PERFORM set_config('sift.enable_agg', agg::text, true);
  EXECUTE 'SELECT coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''') FROM (' || q || ') t' INTO r;
  RETURN r;
EXCEPTION WHEN OTHERS THEN RETURN 'ERROR: ' || SQLERRM;
END $$;
-- result: the rows of the normal plan or its error; same: the scan answers or fails alike; pushed: the plan the scan chose
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
-- The division by the id raises for entity 0, which the join with dims leaves out.
SELECT * FROM check_query($q$
 SELECT sum(c.n) FROM facts f CROSS JOIN LATERAL (SELECT 10 / f.entity_id AS n LIMIT 1) c JOIN dims d ON d.id = f.entity_id AND d.ok
$q$);
-- A cast of text that is no number.
SELECT * FROM check_query($q$
 SELECT sum(c.n) FROM facts f LEFT JOIN LATERAL (SELECT f.code::int AS n LIMIT 1) c ON true JOIN dims d ON d.id = f.entity_id AND d.ok
$q$);
SELECT * FROM check_query($q$
 SELECT d.ok, sum(c.n) FROM facts f JOIN dims d ON d.id = f.entity_id
 JOIN LATERAL (SELECT f.code::int AS n UNION ALL SELECT 0) c ON true GROUP BY 1
$q$);
-- Without the join the rows of entity 0 reach the child, and both plans raise the same error.
SELECT * FROM check_query($q$
 SELECT sum(c.n) FROM facts f CROSS JOIN LATERAL (SELECT 10 / f.entity_id AS n LIMIT 1) c
$q$);
-- A sub-select of the child may return more than one row.
SELECT * FROM check_query($q$
 SELECT sum(c.n) FROM facts f JOIN dims d ON d.id = f.entity_id AND d.ok
 CROSS JOIN LATERAL (SELECT (SELECT t.id FROM tags t WHERE t.entity_id <= f.entity_id) AS n LIMIT 1) c
$q$);
-- Controls: a division by a constant and a comparison cannot raise, so the scan joins the child.
SELECT * FROM check_query($q$
 SELECT c.n, count(*) FROM facts f JOIN dims d ON d.id = f.entity_id AND d.ok
 CROSS JOIN LATERAL (SELECT f.entity_id / 4 AS n WHERE f.entity_id <> 7 LIMIT 1) c GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP FUNCTION run_query(text, bool);
DROP TABLE facts, dims, tags;
DROP EXTENSION pg_sift;
