-- A HAVING condition with a sub-select that reads no aggregate is a condition of the rows of a plain GROUP BY (PostgreSQL moves the ones without a sub-select to WHERE itself):
-- the rows it drops are not grouped, and the sub-select is a semi or anti join the scan answers. `scan` says the aggregate is in the scan, `rows_same` that the rows are those of sift off (the plan may test the
-- condition on the groups instead, as the node does for a sub-select since agg_output_subplan: the rewrite is the plan of the cheaper of the two). What must stay (an aggregate in the condition,
-- a volatile function, grouping sets, no GROUP BY) keeps its HAVING and gives the same rows; a HAVING of several conditions moves the ones of the rows only.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE hv_kinds (id int PRIMARY KEY, enabled bool, grp int);
CREATE TABLE hv_events (id int PRIMARY KEY, kind_id int, region text, qty int, amount float8) WITH (autovacuum_enabled = false);
CREATE TABLE hv_empty (id int PRIMARY KEY, kind_id int, qty int) WITH (autovacuum_enabled = false);
INSERT INTO hv_kinds SELECT i, i % 3 <> 0, i % 4 FROM generate_series(1, 12) i;
INSERT INTO hv_kinds VALUES (20, NULL, NULL);
INSERT INTO hv_events SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE 1 + i % 14 END, CASE WHEN i % 53 = 0 THEN NULL ELSE 'r' || (1 + i % 5) END, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 50 END,
  (i % 90) / 9.0 FROM generate_series(1, 40000) i;
CREATE INDEX hv_events_col ON hv_events USING sift_col (id, kind_id, region, qty, amount);
CREATE INDEX hv_empty_col ON hv_empty USING sift_col (id, kind_id, qty);
VACUUM (FREEZE, ANALYZE) hv_kinds, hv_events, hv_empty;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('IN list',                  $q$SELECT kind_id, count(*), sum(qty) FROM hv_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled)$q$),
  ('NOT IN list',              $q$SELECT kind_id, count(*), sum(qty) FROM hv_events GROUP BY 1 HAVING kind_id NOT IN (SELECT id FROM hv_kinds WHERE grp = 1)$q$),
  ('NOT IN with a NULL',       $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING kind_id NOT IN (SELECT grp FROM hv_kinds)$q$),
  ('EXISTS',                   $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING EXISTS (SELECT 1 FROM hv_kinds k WHERE k.id = hv_events.kind_id AND k.enabled)$q$),
  ('NOT EXISTS',               $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING NOT EXISTS (SELECT 1 FROM hv_kinds k WHERE k.id = hv_events.kind_id AND k.enabled)$q$),
  ('scalar sub-select',        $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING kind_id > (SELECT avg(id) FROM hv_kinds)$q$),
  ('scalar of the key',        $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING (SELECT count(*) FROM hv_kinds k WHERE k.id = hv_events.kind_id) = 0 OR kind_id = 1$q$),
  ('a key and an aggregate',   $q$SELECT kind_id, count(*), sum(qty) FROM hv_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled) AND sum(qty) > 10000$q$),
  ('two conditions of rows',   $q$SELECT kind_id, region, count(*) FROM hv_events GROUP BY 1, 2 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE grp = 2) AND region IN (SELECT 'r' || id FROM hv_kinds WHERE id < 4)$q$),
  ('a key expression',         $q$SELECT kind_id % 3, count(*) FROM hv_events GROUP BY 1 HAVING kind_id % 3 IN (SELECT grp FROM hv_kinds WHERE grp IS NOT NULL)$q$),
  ('NULL keys',                $q$SELECT region, count(*) FROM hv_events GROUP BY 1 HAVING region IS NULL OR region IN (SELECT 'r' || id FROM hv_kinds WHERE id < 3)$q$),
  ('with a WHERE',             $q$SELECT kind_id, sum(amount) FROM hv_events WHERE qty > 10 GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled)$q$),
  ('ORDER BY and LIMIT',       $q$SELECT kind_id, count(*) c FROM hv_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled) ORDER BY c DESC, 1 LIMIT 3$q$),
  ('a window over the groups', $q$SELECT kind_id, count(*), rank() OVER (ORDER BY count(*)) FROM hv_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled)$q$),
  ('over a join',              $q$SELECT k.grp, count(*), sum(e.qty) FROM hv_events e JOIN hv_kinds k ON k.id = e.kind_id GROUP BY 1 HAVING k.grp IN (SELECT grp FROM hv_kinds WHERE id > 8)$q$),
  ('empty table',              $q$SELECT kind_id, count(*) FROM hv_empty GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds)$q$),
  ('an aggregate in the sub-select', $q$SELECT kind_id, sum(qty) FROM hv_events GROUP BY 1 HAVING (SELECT count(*) FROM hv_kinds WHERE id <= sum(qty) / 100) > 2$q$),
  ('an aggregate of the query',      $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING count(*) > (SELECT count(*) / 14 FROM hv_events)$q$),
  ('a volatile function',      $q$SELECT kind_id, count(*) FROM hv_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE random() < 2)$q$),
  ('grouping sets',            $q$SELECT kind_id, region, count(*) FROM hv_events GROUP BY ROLLUP (kind_id, region) HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled)$q$),
  ('grouping sets, NULL keys', $q$SELECT kind_id, region, count(*) FROM hv_events GROUP BY GROUPING SETS ((kind_id), (region), ()) HAVING kind_id IS NULL OR kind_id IN (SELECT id FROM hv_kinds WHERE enabled)$q$),
  ('no GROUP BY',              $q$SELECT count(*), sum(qty) FROM hv_events HAVING 1 IN (SELECT id FROM hv_kinds WHERE id > 100)$q$),
  ('no GROUP BY, true',        $q$SELECT count(*), sum(qty) FROM hv_events HAVING 1 IN (SELECT id FROM hv_kinds WHERE id < 100)$q$),
  ('a sub-query of the form',  $q$SELECT t.kind_id, t.c FROM (SELECT kind_id, count(*) c FROM hv_events GROUP BY 1 HAVING kind_id IN (SELECT id FROM hv_kinds WHERE enabled)) t WHERE t.c > 100$q$);
CREATE FUNCTION having_check(q text) RETURNS TABLE (scan bool, rows_same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  scan := plan LIKE '%Custom Scan (SiftAgg)%';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  rows_same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT q.name, c.* FROM q, LATERAL having_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION having_check(text);
DROP TABLE q, hv_kinds, hv_events, hv_empty;
DROP EXTENSION pg_sift;
