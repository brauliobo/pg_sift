-- The rows the scan keeps of a correlated child are those of its plan for every distinct input, bounded by sift.agg_lookup_max_rows. The inputs are not
-- bounded by the scanned table's rows when the child reads a column of another joined table that has several rows for each of them: the query keeps the
-- normal plan when they exceed the setting, and runs in the scan when the inputs are the scanned table's own.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, entity_id int, amount float8);
INSERT INTO facts SELECT i, i, i FROM generate_series(1, 100) i;
CREATE INDEX facts_col ON facts USING sift_col (id, entity_id, amount);
CREATE TABLE items (entity_id int, v int);
INSERT INTO items SELECT 1 + i % 100, i FROM generate_series(1, 20000) i;
CREATE INDEX ON items (entity_id);
VACUUM (FREEZE, ANALYZE) facts, items;
SET sift.agg_lookup_max_rows = 1000;
CREATE FUNCTION plan_has_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; found bool := false;
BEGIN
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := found OR l LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  RETURN found;
END $$;
-- 20,000 distinct values of items.v reach the child through 100 scanned rows
SELECT plan_has_scan($q$
 SELECT c.w % 7 AS k, count(*) AS n FROM facts f JOIN items e ON e.entity_id = f.entity_id
 JOIN LATERAL (SELECT e.v * 2 AS w UNION ALL SELECT -e.v) c ON true GROUP BY 1
$q$) AS fanned_out;
-- the scanned table's own column: 100 inputs
SELECT plan_has_scan($q$
 SELECT c.w % 7 AS k, count(*) AS n FROM facts f JOIN items e ON e.entity_id = f.entity_id
 JOIN LATERAL (SELECT f.id * 2 AS w UNION ALL SELECT -f.id) c ON true GROUP BY 1
$q$) AS scanned_column;
DROP FUNCTION plan_has_scan(text);
DROP TABLE facts, items;
DROP EXTENSION pg_sift;
