-- A HAVING condition on the groups in every shape of query that groups: no GROUP BY (one group, there even when no row is), one or several keys, grouping sets (the condition
-- reads grouping() and the NULL keys of the super-aggregates), joins to a lookup, top-N (ORDER BY an aggregate, LIMIT, OFFSET, WITH TIES: the HAVING is applied before the
-- ranking, so a group it drops never takes a place), DISTINCT, window functions over the groups that pass, and a grouped sub-query read by an outer query. `ordered` compares the rows
-- in the order the statement returns them, where its ORDER BY is a total order.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
\ir test/agg_having_helpers.sql

CREATE TABLE q (ord serial, shape text, name text, sql text, ordered bool DEFAULT false);
INSERT INTO q (shape, name, sql) VALUES
  -- no GROUP BY: one group, which a HAVING keeps or drops
  ('no GROUP BY', 'count passes',          $q$SELECT count(*) AS c FROM hv_events HAVING count(*) > 100$q$),
  ('no GROUP BY', 'count fails',           $q$SELECT count(*) AS c FROM hv_events HAVING count(*) < 100$q$),
  ('no GROUP BY', 'two aggregates, not selected', $q$SELECT 1 AS one FROM hv_events HAVING sum(counter_a) > 1 AND max(id) < 100000 AND min(id) = 1$q$),
  ('no GROUP BY', 'a sum that is NULL',    $q$SELECT sum(counter_a) AS s FROM hv_events WHERE id < 0 HAVING sum(counter_a) IS NULL$q$),
  ('no GROUP BY', 'a sum, the HAVING fails on NULL', $q$SELECT sum(counter_a) AS s FROM hv_events WHERE id < 0 HAVING sum(counter_a) > 0$q$),
  ('no GROUP BY', 'an empty table, count = 0', $q$SELECT count(*) AS c FROM hv_empty HAVING count(*) = 0$q$),
  ('no GROUP BY', 'an empty table, count > 0', $q$SELECT count(*) AS c FROM hv_empty HAVING count(*) > 0$q$),
  ('no GROUP BY', 'an empty table, NULL sum', $q$SELECT sum(counter_a) AS s FROM hv_empty HAVING sum(counter_a) IS NULL$q$),
  ('no GROUP BY', 'a distinct count',      $q$SELECT count(DISTINCT counter_a) AS d FROM hv_events HAVING count(DISTINCT counter_a) = 6$q$),
  ('no GROUP BY', 'a percentile',          $q$SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY counter_b) AS p FROM hv_events HAVING percentile_cont(0.5) WITHIN GROUP (ORDER BY counter_b) = 4$q$),
  ('no GROUP BY', 'GROUP BY ()',           $q$SELECT count(*) AS c FROM hv_events GROUP BY () HAVING count(*) > 3$q$),
  -- GROUP BY with no row at all
  ('empty input', 'a table with no row',   $q$SELECT tenant_id, count(*) AS c FROM hv_empty GROUP BY 1 HAVING count(*) >= 0$q$),
  ('empty input', 'a WHERE that drops every row', $q$SELECT tenant_id, sum(counter_a) AS s FROM hv_events WHERE id < 0 GROUP BY 1 HAVING sum(counter_a) IS NULL$q$),
  ('empty input', 'ROLLUP of an empty table', $q$SELECT tenant_id, count(*) AS c FROM hv_empty GROUP BY ROLLUP (tenant_id) HAVING count(*) = 0$q$),
  ('empty input', 'grouping sets with () and no row', $q$SELECT category_id, count(*) AS c FROM hv_events WHERE id < 0 GROUP BY GROUPING SETS ((category_id), ()) HAVING count(*) = 0$q$),
  -- several keys
  ('several keys', 'two keys and a count', $q$SELECT tenant_id, category_id, count(*) AS c FROM hv_events GROUP BY 1, 2 HAVING count(*) > 400$q$),
  ('several keys', 'a text and a date key', $q$SELECT kind, event_date, sum(counter_b) AS s FROM hv_events GROUP BY 1, 2 HAVING sum(counter_b) > 300$q$),
  ('several keys', 'a date key',           $q$SELECT event_date, sum(counter_b) AS s FROM hv_events GROUP BY 1 HAVING sum(counter_b) > 4500$q$),
  ('several keys', 'a numeric key',        $q$SELECT value_num, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 55$q$),
  ('several keys', 'a boolean key',        $q$SELECT flag_a, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 20000$q$),
  ('several keys', 'a key expression (a month)', $q$SELECT date_trunc('month', event_date) AS m, sum(counter_b) AS s FROM hv_events GROUP BY 1 HAVING sum(counter_b) > 5000$q$),
  ('several keys', 'a key divided',        $q$SELECT counter_b / 3 AS g, sum(amount) AS s FROM hv_events GROUP BY 1 HAVING sum(amount) > 20000 AND counter_b / 3 > 0$q$),
  ('several keys', 'a coalesced text key', $q$SELECT coalesce(kind, 'none') AS k, count(*) AS c FROM hv_events GROUP BY 1 HAVING coalesce(kind, 'none') <> 'k1' AND count(*) > 100$q$),
  -- grouping sets
  ('grouping sets', 'ROLLUP and a count',  $q$SELECT category_id, kind, count(*) AS c FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING count(*) > 1000$q$),
  ('grouping sets', 'ROLLUP, grouping() = 1', $q$SELECT category_id, kind, count(*) AS c, grouping(category_id, kind) AS g FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING grouping(category_id, kind) = 1 AND count(*) > 1000$q$),
  ('grouping sets', 'ROLLUP, one grouping()', $q$SELECT category_id, kind, count(*) AS c FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING grouping(category_id) = 0 AND count(*) > 1000$q$),
  ('grouping sets', 'ROLLUP, a NULL key',  $q$SELECT category_id, kind, count(*) AS c FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING category_id IS NULL$q$),
  ('grouping sets', 'CUBE and a sum',      $q$SELECT category_id, status, sum(counter_a) AS s FROM hv_events GROUP BY CUBE (category_id, status) HAVING sum(counter_a) > 3000$q$),
  ('grouping sets', 'CUBE, grouping() in a set', $q$SELECT category_id, status, sum(counter_a) AS s, grouping(category_id, status) AS g FROM hv_events GROUP BY CUBE (category_id, status) HAVING grouping(category_id, status) IN (1, 2) AND sum(counter_a) > 100$q$),
  ('grouping sets', 'CUBE, sum of two grouping()', $q$SELECT category_id, status, grouping(category_id) AS g1, grouping(status) AS g2 FROM hv_events GROUP BY CUBE (category_id, status) HAVING grouping(category_id) + grouping(status) = 1$q$),
  ('grouping sets', 'sets with ()',        $q$SELECT category_id, status, count(*) AS c FROM hv_events GROUP BY GROUPING SETS ((category_id), (status), ()) HAVING count(*) > 5000$q$),
  ('grouping sets', 'the grand total only', $q$SELECT count(*) AS c FROM hv_events GROUP BY GROUPING SETS (()) HAVING count(*) > 3$q$),
  ('grouping sets', 'a distinct count',    $q$SELECT category_id, kind, count(DISTINCT counter_a) AS d FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING count(DISTINCT counter_a) > 3$q$),
  ('grouping sets', 'a percentile',        $q$SELECT category_id, kind, percentile_cont(0.5) WITHIN GROUP (ORDER BY counter_b) AS p FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING percentile_cont(0.5) WITHIN GROUP (ORDER BY counter_b) >= 4$q$),
  ('grouping sets', 'a sub-select on a key', $q$SELECT category_id, status, count(*) AS c FROM hv_events GROUP BY CUBE (category_id, status) HAVING category_id IN (SELECT id FROM hv_tenants WHERE tier = 1) AND count(*) > 10$q$),
  ('grouping sets', 'over a join',         $q$SELECT t.tier, e.status, count(*) AS c FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY ROLLUP (t.tier, e.status) HAVING count(*) > 1000$q$),
  -- joins to a lookup
  ('join', 'group by a lookup column',     $q$SELECT t.tier, count(*) AS c, sum(e.counter_a) AS s FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY 1 HAVING sum(e.counter_a) > 1000$q$),
  ('join', 'a label key and a count',      $q$SELECT t.label, count(*) AS c FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY 1 HAVING count(*) > 2000 AND t.label <> 'tenant_1'$q$),
  ('join', 'an aggregate of the lookup',   $q$SELECT t.tier, max(t.id) AS m, count(*) AS c FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY 1 HAVING max(t.id) > 12$q$),
  ('join', 'distinct count of the fact key', $q$SELECT t.tier FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY 1 HAVING count(DISTINCT e.tenant_id) > 3 AND sum(e.counter_a) > 0$q$),
  ('join', 'a WHERE on the lookup',        $q$SELECT t.tier, count(*) AS c FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id WHERE t.active GROUP BY 1 HAVING count(*) > 100$q$),
  ('join', 'a LEFT JOIN, groups with no match', $q$SELECT t.id, count(e.id) AS c FROM hv_tenants t LEFT JOIN hv_events e ON e.tenant_id = t.id GROUP BY 1 HAVING count(e.id) = 0$q$),
  ('join', 'a LEFT JOIN, a sum that is NULL', $q$SELECT t.id, sum(e.counter_a) AS s FROM hv_tenants t LEFT JOIN hv_events e ON e.tenant_id = t.id GROUP BY 1 HAVING sum(e.counter_a) IS NULL$q$),
  ('join', 'a semi join in the WHERE',     $q$SELECT tenant_id, count(*) AS c FROM hv_events e WHERE EXISTS (SELECT 1 FROM hv_tenants t WHERE t.id = e.tenant_id AND t.tier = 0) GROUP BY 1 HAVING count(*) > 100$q$),
  ('join', 'a self join',                  $q$SELECT a.tenant_id, count(*) AS c FROM hv_events a JOIN hv_events b ON b.id = a.id + 1 AND b.tenant_id = a.tenant_id GROUP BY 1 HAVING count(*) > 100$q$),
  ('sub-select', 'a sub-select on the key and an aggregate', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id IN (SELECT id FROM hv_tenants WHERE active) AND count(*) > 1000$q$),
  ('sub-select', 'a sub-select that compares the count', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > (SELECT count(*) / 13 FROM hv_events)$q$),
  ('sub-select', 'ANY of a sub-select of counts', $q$SELECT tenant_id FROM hv_events GROUP BY 1 HAVING count(*) = ANY (SELECT unnest(ARRAY[5, 8, 990]))$q$),
  ('sub-select', 'every count against a sub-select', $q$SELECT tenant_id FROM hv_events GROUP BY 1 HAVING count(*) >= ALL (SELECT count(*) FROM hv_events GROUP BY tenant_id)$q$),
  ('lateral', 'a lateral group with its own HAVING', $q$SELECT t.id, g.c FROM hv_tenants t, LATERAL (SELECT count(*) AS c FROM hv_events e WHERE e.tenant_id = t.id GROUP BY e.category_id HAVING count(*) > 100) g$q$),
  -- DISTINCT, UNION, a nested query
  ('DISTINCT', 'DISTINCT of the counts',   $q$SELECT DISTINCT count(*) AS c FROM hv_events GROUP BY tenant_id HAVING count(*) > 100$q$),
  ('DISTINCT', 'DISTINCT over CUBE',       $q$SELECT DISTINCT count(*) AS c FROM hv_events GROUP BY CUBE (category_id, status) HAVING count(*) > 100$q$),
  ('UNION', 'two arms with a HAVING each', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 2500 UNION ALL SELECT tenant_id, count(*) FROM hv_events GROUP BY 1 HAVING count(*) < 4$q$),
  ('nested', 'a HAVING inside, a count outside', $q$WITH g AS (SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 100) SELECT count(*) AS n, sum(c) AS s FROM g$q$),
  ('nested', 'a WHERE outside on an aggregate inside', $q$SELECT * FROM (SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1) g WHERE c > 100$q$),
  ('nested', 'a HAVING inside a HAVING', $q$SELECT category_id, count(*) AS n FROM (SELECT tenant_id, category_id, sum(counter_a) AS s FROM hv_events GROUP BY 1, 2 HAVING sum(counter_a) > 100) x GROUP BY 1 HAVING count(*) > 2$q$),
  ('nested', 'a join of the groups to the rows', $q$SELECT o.tenant_id, count(*) AS n FROM hv_events o JOIN (SELECT tenant_id FROM hv_events GROUP BY 1 HAVING count(*) > 5) s ON o.tenant_id = s.tenant_id GROUP BY 1$q$),
  ('nested', 'groups in an IN of the WHERE', $q$SELECT count(*) AS n FROM hv_events WHERE tenant_id IN (SELECT tenant_id FROM hv_events WHERE event_date >= DATE '2026-01-20' GROUP BY 1 HAVING count(*) > 20)$q$),
  ('nested', 'LIMIT inside, no ORDER BY', $q$SELECT count(*) AS n FROM (SELECT tenant_id FROM hv_events GROUP BY 1 HAVING count(*) > 100 LIMIT 3) x$q$),
  -- window functions over the groups that pass
  ('window', 'rank over the counts',       $q$SELECT tenant_id, count(*) AS c, rank() OVER (ORDER BY count(*) DESC) AS r FROM hv_events GROUP BY 1 HAVING sum(counter_a) > 100$q$),
  ('window', 'row_number and lag',         $q$SELECT tenant_id, row_number() OVER (ORDER BY sum(counter_a) DESC NULLS LAST, tenant_id) AS rn, lag(count(*)) OVER (ORDER BY tenant_id) AS prev FROM hv_events GROUP BY 1 HAVING max(counter_b) = 8$q$),
  ('window', 'a running sum of the groups', $q$SELECT tenant_id, c, sum(c) OVER (ORDER BY tenant_id) AS run FROM (SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 100) g$q$),
  ('window', 'the total of the groups that pass', $q$SELECT tenant_id, sum(sum(counter_a)) OVER () AS total FROM hv_events GROUP BY 1 HAVING sum(counter_a) IS NOT NULL$q$);
INSERT INTO q (shape, name, sql, ordered) VALUES
  -- top-N: the HAVING first, then the order and the cut
  ('top-N', 'ORDER BY an aggregate, LIMIT', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 100 ORDER BY c DESC, 1 LIMIT 3$q$, true),
  ('top-N', 'OFFSET, NULLS LAST',          $q$SELECT tenant_id, sum(counter_a) AS s FROM hv_events GROUP BY 1 HAVING sum(counter_a) > 100 ORDER BY s DESC NULLS LAST, 1 LIMIT 3 OFFSET 1$q$, true),
  ('top-N', 'ascending, ties on the count', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) < 3000 ORDER BY c, tenant_id LIMIT 5$q$, true),
  ('top-N', 'ORDER BY an aggregate not selected', $q$SELECT tenant_id FROM hv_events GROUP BY 1 HAVING count(*) > 100 ORDER BY max(counter_b) DESC, tenant_id LIMIT 3$q$, true),
  ('top-N', 'ORDER BY the key',            $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 100 ORDER BY tenant_id DESC LIMIT 2$q$, true),
  ('top-N', 'HAVING drops the largest group', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) < 3000 ORDER BY c DESC, 1 LIMIT 2$q$, true),
  ('top-N', 'HAVING on the ranked aggregate', $q$SELECT tenant_id, sum(counter_b) AS s FROM hv_events GROUP BY 1 HAVING sum(counter_b) BETWEEN 3000 AND 9000 ORDER BY s DESC, 1 LIMIT 4$q$, true),
  ('top-N', 'fewer groups than the LIMIT', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 2000 ORDER BY c DESC, 1 LIMIT 50$q$, true),
  ('top-N', 'ROLLUP, ORDER BY and LIMIT',  $q$SELECT category_id, kind, count(*) AS c FROM hv_events GROUP BY ROLLUP (category_id, kind) HAVING count(*) > 100 ORDER BY c DESC, 1, 2 LIMIT 4$q$, true),
  ('top-N', 'a join, ORDER BY and LIMIT',  $q$SELECT t.tier, count(*) AS c FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id GROUP BY 1 HAVING count(*) > 100 ORDER BY c DESC, 1 LIMIT 2$q$, true),
  ('top-N', 'a key condition, ORDER BY, LIMIT', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id > 8 AND count(*) > 100 ORDER BY c DESC, 1 LIMIT 3$q$, true),
  ('top-N', 'inside, with the outer ORDER BY', $q$SELECT tenant_id FROM (SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 5 ORDER BY c DESC LIMIT 5) x ORDER BY 1$q$, true);
INSERT INTO q (shape, name, sql) VALUES
  ('top-N', 'WITH TIES at the cut',        $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 100 ORDER BY c DESC FETCH FIRST 4 ROWS WITH TIES$q$),
  ('top-N', 'WITH TIES, one row',          $q$SELECT category_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 100 ORDER BY c DESC FETCH FIRST 1 ROW WITH TIES$q$);
SELECT q.shape, q.name, c.* FROM q, LATERAL having_run(q.sql, q.ordered) c ORDER BY q.ord;

DROP TABLE q, hv_events, hv_tenants, hv_empty;
DROP FUNCTION having_plan(text), having_rows(text, bool, bool), having_run(text, bool), having_same(text, text, text);
DROP EXTENSION pg_sift;
