-- A table left-joined to a join of two small tables (every kind and region with its count, zero for the pairs of no event) cannot be one preserved lookup, so the join read every row of
-- the table. The scan makes the groups of the rows by the columns the join equates and the query joins them to the pairs: a count is the group's (0 for a pair of none), a sum, min or max
-- the group's (NULL for none), count(*) counts the row of a pair of none once. `partial` says the plan reads the groups in the scan, `rows_same` that the rows are those of sift off.
-- What the groups cannot give (a float sum, avg, count(DISTINCT), a column of the table outside the aggregates, a WHERE on it) keeps the join.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE lp_kinds (id int PRIMARY KEY, name text NOT NULL);
CREATE TABLE lp_regions (region text PRIMARY KEY);
CREATE TABLE lp_events (id int PRIMARY KEY, kind_id int, region text, status text NOT NULL, qty int, amount numeric, ratio float8) WITH (autovacuum_enabled = false);
INSERT INTO lp_kinds SELECT i, 'kind_' || i FROM generate_series(1, 8) i;
INSERT INTO lp_regions SELECT 'r' || i FROM generate_series(1, 6) i;
INSERT INTO lp_events SELECT i, CASE WHEN i % 89 = 0 THEN NULL ELSE 1 + i % 6 END, CASE WHEN i % 97 = 0 THEN NULL ELSE 'r' || (1 + i % 5) END,
  (ARRAY['new','paid','void'])[1 + i % 3], CASE WHEN i % 13 = 0 THEN NULL ELSE i % 50 END, (i % 1000) / 10.0, (i % 70) / 7.0 FROM generate_series(1, 60000) i;
CREATE INDEX lp_events_col ON lp_events USING sift_col (id, kind_id, region, status, qty, amount, ratio);
VACUUM (FREEZE, ANALYZE) lp_kinds, lp_regions, lp_events;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('count of a column',           $q$SELECT k.name, r.region, count(e.id) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2$q$),
  ('count(*) and sums',           $q$SELECT k.name, r.region, count(*), sum(e.qty), sum(e.amount), min(e.amount), max(e.status) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2$q$),
  ('a condition on the table',    $q$SELECT k.name, r.region, count(e.id), sum(e.qty) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region AND e.status = 'paid' GROUP BY 1, 2$q$),
  ('a condition on the pairs',    $q$SELECT k.name, r.region, count(*), count(e.id) FROM lp_kinds k JOIN lp_regions r ON true LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region AND k.id % 2 = 0 GROUP BY 1, 2$q$),
  ('a WHERE on the pairs',        $q$SELECT k.name, r.region, count(e.id) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region WHERE k.id <= 7 AND r.region <> 'r2' GROUP BY 1, 2$q$),
  ('groups of several pairs',     $q$SELECT k.name, count(e.id), count(*), sum(e.qty) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1$q$),
  ('HAVING',                      $q$SELECT k.name, r.region, sum(e.qty) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2 HAVING sum(e.qty) > 5000 OR count(e.id) = 0$q$),
  ('no GROUP BY',                 $q$SELECT count(*), count(e.id), sum(e.qty), max(e.amount) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region$q$),
  ('no GROUP BY, no pair',        $q$SELECT count(*), count(e.id), sum(e.qty) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region WHERE k.id > 100$q$),
  ('ORDER BY and LIMIT',          $q$SELECT k.name, r.region, count(e.id) c FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2 ORDER BY c DESC, 1, 2 LIMIT 5$q$),
  ('a float sum',                 $q$SELECT k.name, r.region, sum(e.ratio) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2$q$),
  ('avg',                         $q$SELECT k.name, r.region, avg(e.qty) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2$q$),
  ('count(DISTINCT)',             $q$SELECT k.name, r.region, count(DISTINCT e.status) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2$q$),
  ('a column of the table',       $q$SELECT k.name, e.status, count(*) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region GROUP BY 1, 2$q$),
  ('a WHERE on the table',        $q$SELECT k.name, r.region, count(e.id) FROM lp_kinds k CROSS JOIN lp_regions r LEFT JOIN lp_events e ON e.kind_id = k.id AND e.region = r.region WHERE e.status IS NULL OR e.status = 'void' GROUP BY 1, 2$q$);
CREATE FUNCTION partial_check(q text) RETURNS TABLE (partial bool, rows_same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  partial := plan LIKE '%Subquery Scan on e%' AND plan LIKE '%Custom Scan (SiftAgg)%';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  rows_same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT q.name, c.* FROM q, LATERAL partial_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION partial_check(text);
DROP TABLE q, lp_kinds, lp_regions, lp_events;
DROP EXTENSION pg_sift;
