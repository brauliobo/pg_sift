-- The same HAVING statements under the conditions that change HOW the scan answers them, not what they ask: from the pre-aggregates of the units or by reading the rows
-- (sift.agg_preagg), by one process or by workers, on a table that has taken inserts, updates and deletes since its index was built (the write buffer and the tombstones: a group
-- crosses the HAVING bound because of a row that is not in a unit) and after the VACUUM that folds them in, and with parameters (a generic plan has $n in the condition of the
-- groups and in the condition of the rows, a custom plan has the values). Each section runs the list and compares every row with sift off. `workers` is the number of workers the plan
-- asks for.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
\ir test/agg_having_helpers.sql

CREATE FUNCTION having_workers(q text) RETURNS int LANGUAGE sql AS
$$ SELECT coalesce(substring(having_plan(q) from 'Workers Planned: (\d+)')::int, 0) $$;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('count',               $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 2000$q$),
  ('count at the boundary',$q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) BETWEEN 5 AND 8$q$),
  ('sum, NULL groups',    $q$SELECT tenant_id, sum(counter_a) AS s FROM hv_events GROUP BY 1 HAVING sum(counter_a) IS NULL OR sum(counter_a) >= 5000$q$),
  ('avg',                 $q$SELECT tenant_id, avg(counter_a) AS a FROM hv_events GROUP BY 1 HAVING avg(counter_a) < 2$q$),
  ('max = min',           $q$SELECT tenant_id, max(counter_a) AS mx FROM hv_events GROUP BY 1 HAVING max(counter_a) = min(counter_a)$q$),
  ('count distinct',      $q$SELECT category_id, count(DISTINCT counter_a) AS d FROM hv_events GROUP BY 1 HAVING count(DISTINCT counter_a) > 3$q$),
  ('aggregates not selected', $q$SELECT kind FROM hv_events GROUP BY 1 HAVING max(event_date) > DATE '2026-01-29' AND min(value_num) < 1$q$),
  ('FILTER',              $q$SELECT tenant_id, count(*) FILTER (WHERE status = 1) AS c FROM hv_events GROUP BY 1 HAVING count(*) FILTER (WHERE status = 1) > 700$q$),
  ('percentile',          $q$SELECT category_id FROM hv_events GROUP BY 1 HAVING percentile_cont(0.5) WITHIN GROUP (ORDER BY counter_b) > 3.9$q$),
  ('a key and an aggregate', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id > 8 AND count(*) > 2000$q$),
  ('a WHERE and a HAVING',$q$SELECT tenant_id, sum(counter_b) AS s FROM hv_events WHERE counter_b > 4 GROUP BY 1 HAVING sum(counter_b) > 5000$q$),
  ('a sub-select on the key', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id IN (SELECT id FROM hv_tenants WHERE active) AND count(*) > 1000$q$),
  ('rollup and grouping()', $q$SELECT category_id, kind, count(*) AS c, grouping(category_id, kind) AS g FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING grouping(category_id, kind) < 3 AND count(*) > 1000$q$),
  ('a join to a lookup',  $q$SELECT t.tier, count(*) AS c, sum(e.counter_a) AS s FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY 1 HAVING sum(e.counter_a) > 1000$q$),
  ('no GROUP BY',         $q$SELECT count(*) AS c, sum(counter_a) AS s FROM hv_events HAVING sum(counter_a) > 100$q$),
  ('top-N',               $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 5 ORDER BY c DESC, 1 LIMIT 4$q$);

SELECT 'default' AS mode, q.name, having_workers(q.sql) AS workers, c.* FROM q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;

SET sift.agg_preagg = off;
SELECT 'rows, no pre-aggregates' AS mode, q.name, having_workers(q.sql) AS workers, c.* FROM q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT 'two workers' AS mode, q.name, having_workers(q.sql) AS workers, c.* FROM q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;
SET max_parallel_workers_per_gather = 0;
RESET sift.agg_preagg;

-- inserts, updates and deletes that move groups across the bounds: 15 goes from 5 to 8 rows and gets a value, 16 from 1 to 7, 17 loses a row, 18 is new, the biggest group loses seven rows
INSERT INTO hv_events SELECT 40000 + i, 15, 2, 'k2', DATE '2026-02-05', 1, 3, 5, 2.50, 2.5, 'l3', true FROM generate_series(1, 3) i;
INSERT INTO hv_events SELECT 41000 + i, 16, 2, 'k2', DATE '2026-02-02', 1, 9, 9, 9.00, 9.0, 'l1', true FROM generate_series(1, 6) i;
INSERT INTO hv_events VALUES (42000, 18, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
DELETE FROM hv_events WHERE id = 30007;
DELETE FROM hv_events WHERE id IN (SELECT id FROM hv_events WHERE tenant_id = 1 ORDER BY id LIMIT 7);
UPDATE hv_events SET tenant_id = 15, counter_a = 11 WHERE id IN (SELECT id FROM hv_events WHERE tenant_id IS NULL ORDER BY id LIMIT 4);
UPDATE hv_events SET counter_b = counter_b + 1 WHERE tenant_id = 13 AND id % 2 = 0;
SELECT 'after writes' AS mode, q.name, having_workers(q.sql) AS workers, c.* FROM q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;

SET sift.agg_preagg = off;
SET max_parallel_workers_per_gather = 2;
SELECT 'after writes, two workers, rows' AS mode, q.name, having_workers(q.sql) AS workers, c.* FROM q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;
SET max_parallel_workers_per_gather = 0;
RESET sift.agg_preagg;

VACUUM (ANALYZE) hv_events;
SELECT 'after VACUUM' AS mode, q.name, having_workers(q.sql) AS workers, c.* FROM q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;

-- parameters: a generic plan keeps $n in the HAVING (groups) and in the WHERE (rows), and in the sub-select; two calls for each, one that keeps groups and one that keeps none
PREPARE h_count(int, bigint) AS SELECT tenant_id, count(*) AS c FROM hv_events WHERE status = $1 GROUP BY 1 HAVING count(*) > $2;
PREPARE h_key_sum(int, numeric) AS SELECT tenant_id, sum(counter_b) AS s FROM hv_events GROUP BY 1 HAVING tenant_id > $1 AND sum(counter_b) > $2;
PREPARE h_text(text, bigint) AS SELECT kind, count(*) AS c FROM hv_events GROUP BY 1 HAVING kind <> $1 AND count(*) > $2;
PREPARE h_date(date, int) AS SELECT kind, max(event_date) AS d FROM hv_events GROUP BY 1 HAVING max(event_date) > $1 AND min(counter_b) = $2;
PREPARE h_sub(smallint, bigint) AS SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id IN (SELECT id FROM hv_tenants WHERE tier = $1) AND count(*) > $2;
PREPARE h_filter(int, bigint) AS SELECT tenant_id, count(*) FILTER (WHERE status = $1) AS c FROM hv_events GROUP BY 1 HAVING count(*) FILTER (WHERE status = $1) > $2;
PREPARE h_array(int[]) AS SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) = ANY ($1);
PREPARE h_none(bigint) AS SELECT count(*) AS c, sum(counter_a) AS s FROM hv_events HAVING count(*) > $1;
PREPARE h_topn(bigint, bigint) AS SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > $1 ORDER BY c DESC, 1 LIMIT $2;
CREATE TABLE qp (ord serial, name text, sql text);
INSERT INTO qp (name, sql) VALUES
  ('count, rows kept',        'EXECUTE h_count(1, 700)'),
  ('count, none kept',        'EXECUTE h_count(1, 99999)'),
  ('key and sum, kept',       'EXECUTE h_key_sum(8, 5000)'),
  ('key and sum, none kept',  'EXECUTE h_key_sum(100, 5000)'),
  ('text key, kept',          'EXECUTE h_text(''k1'', 1000)'),
  ('text key, NULL parameter','EXECUTE h_text(NULL, 1000)'),
  ('max date and min',        'EXECUTE h_date(DATE ''2026-01-29'', 0)'),
  ('sub-select, kept',        'EXECUTE h_sub(1::smallint, 100)'),
  ('sub-select, none kept',   'EXECUTE h_sub(9::smallint, 100)'),
  ('FILTER, kept',            'EXECUTE h_filter(1, 700)'),
  ('FILTER, none kept',       'EXECUTE h_filter(1, 99999)'),
  ('ANY of an array',         'EXECUTE h_array(ARRAY[5, 8, 990, 2969])'),
  ('ANY of an empty array',   'EXECUTE h_array(ARRAY[]::int[])'),
  ('no GROUP BY, kept',       'EXECUTE h_none(100)'),
  ('no GROUP BY, dropped',    'EXECUTE h_none(99999)'),
  ('top-N',                   'EXECUTE h_topn(5, 3)');
SET plan_cache_mode = force_generic_plan;
SELECT 'generic plan' AS mode, q.name, c.* FROM qp q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;
SET plan_cache_mode = force_custom_plan;
SELECT 'custom plan' AS mode, q.name, c.* FROM qp q, LATERAL having_run(q.sql, q.name = 'top-N') c ORDER BY q.ord;
RESET plan_cache_mode;
DEALLOCATE ALL;

DROP TABLE q, qp, hv_events, hv_tenants, hv_empty;
DROP FUNCTION having_workers(text), having_plan(text), having_rows(text, bool, bool), having_run(text, bool), having_same(text, text, text);
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
DROP EXTENSION pg_sift;
