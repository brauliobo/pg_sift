-- A correlated child that the scan probes (NOT EXISTS, EXISTS or a scalar sub-select that the planner hook made a lateral probe, `sift_probe`) runs for every tuple that reaches the join, the
-- ones a condition on what the child gives (`IS NULL` of an anti join) drops after the child ran too: the probes are priced by the tuples the scan reads, not by the rows it aggregates.
-- Priced by the rows of the result they cost a hundredth of what they take and beat the load of the child's rows into a lookup table, which is the cheaper plan when most rows of the
-- outer table go through a probe. `lookup` says the plan loads the child's rows (a `Lookup Joins:` line without `sift_probe`), `rows` that the rows are those of sift off.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ow (id int PRIMARY KEY, customer_id int NOT NULL, created_at timestamptz NOT NULL, status text NOT NULL) WITH (autovacuum_enabled = false);
CREATE TABLE ow_items (id int PRIMARY KEY, order_id int NOT NULL, qty int NOT NULL, price float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ow SELECT i, 1 + i % 997, timestamptz '2025-01-01' + i * interval '7 minutes', (ARRAY['new','paid','shipped'])[1 + i % 3] FROM generate_series(1, 60000) i;
INSERT INTO ow_items SELECT i, 1 + (i * 7) % 75000, 1 + i % 4, ((i * 37) % 20000) / 100.0 FROM generate_series(1, 150000) i;
CREATE INDEX ow_items_order ON ow_items (order_id);
CREATE INDEX ow_col ON ow USING sift_col (id, customer_id, created_at, status);
CREATE INDEX ow_items_col ON ow_items USING sift_col (id, order_id, qty, price);
VACUUM (FREEZE, ANALYZE) ow, ow_items;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('not exists with a filter',  $q$SELECT count(*) FROM ow o WHERE NOT EXISTS (SELECT 1 FROM ow_items i WHERE i.order_id = o.id AND i.price > 150)$q$),
  ('not exists, grouped',       $q$SELECT o.status, count(*) FROM ow o WHERE NOT EXISTS (SELECT 1 FROM ow_items i WHERE i.order_id = o.id AND i.qty = 4) GROUP BY 1$q$),
  ('exists with a filter',      $q$SELECT count(*) FROM ow o WHERE EXISTS (SELECT 1 FROM ow_items i WHERE i.order_id = o.id AND i.price > 150)$q$);
CREATE FUNCTION probe_check(q text) RETURNS TABLE (engaged bool, lookup bool, probes bool, rows_same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  probes := plan LIKE '%sift_probe%';
  lookup := plan LIKE '%Lookup Joins:%' AND NOT probes;
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  rows_same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT q.name, c.* FROM q, LATERAL probe_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION probe_check(text);
DROP TABLE q, ow, ow_items;
DROP EXTENSION pg_sift;
