-- Two different moments: a WHERE condition applies BEFORE the GROUP BY, to the rows, so it changes what the aggregates add up and which groups exist; a HAVING condition
-- applies AFTER, to the groups, so it only keeps or drops whole groups and never changes an aggregate. The scan answers the first in the scan (`where_in_scan`, the
-- `Filters:` line of the node) and the second on the aggregated groups (`having_on_groups`, its `Filter:` line); a HAVING condition on a group key only is a condition of
-- the rows, which PostgreSQL moves to the WHERE itself. Part 1 lists statements by idea (a: WHERE, b: HAVING, c: both), part 2 sets pairs of statements side by side: the
-- invariants that make the two moments different.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
\ir test/agg_having_helpers.sql

-- part 1: where each condition goes, and the rows (the same as sift off)
CREATE TABLE q (ord serial, idea text, name text, sql text);
INSERT INTO q (idea, name, sql) VALUES
  ('a WHERE (rows)',   'equal on a key',            $q$SELECT tenant_id, count(*) AS c, sum(counter_b) AS s FROM hv_events WHERE tenant_id = 1 GROUP BY 1$q$),
  ('a WHERE (rows)',   'range on a date',           $q$SELECT tenant_id, count(*) AS c, sum(counter_b) AS s FROM hv_events WHERE event_date >= DATE '2026-01-15' GROUP BY 1$q$),
  ('a WHERE (rows)',   'IN a list of a text',       $q$SELECT kind, count(*) AS c, avg(amount) AS a FROM hv_events WHERE kind IN ('k1', 'k3') GROUP BY 1$q$),
  ('a WHERE (rows)',   'on a measure',              $q$SELECT tenant_id, count(*) AS c, sum(counter_a) AS s FROM hv_events WHERE counter_b > 4 GROUP BY 1$q$),
  ('a WHERE (rows)',   'NULL test',                 $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE counter_a IS NULL GROUP BY 1$q$),
  ('a WHERE (rows)',   'OR of two columns',         $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE status = 3 OR flag_a GROUP BY 1$q$),
  ('a WHERE (rows)',   'an expression',             $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE counter_a + counter_b > 9 GROUP BY 1$q$),
  ('a WHERE (rows)',   'drops every row of a group',$q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE tenant_id IN (15, 16, 17) AND counter_a IS NOT NULL GROUP BY 1$q$),
  ('a WHERE (rows)',   'no row left, grouped',      $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE id < 0 GROUP BY 1$q$),
  ('a WHERE (rows)',   'no row left, one group',    $q$SELECT count(*) AS c, sum(counter_a) AS s FROM hv_events WHERE id < 0$q$),
  ('a WHERE (rows)',   'a sub-select',              $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE category_id IN (SELECT id FROM hv_tenants WHERE tier = 1) GROUP BY 1$q$),
  ('b HAVING (groups)','on a count',                $q$SELECT tenant_id, count(*) AS c, sum(counter_b) AS s FROM hv_events GROUP BY 1 HAVING count(*) > 2000$q$),
  ('b HAVING (groups)','on a sum not selected',     $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING sum(counter_b) > 9000$q$),
  ('b HAVING (groups)','on an average',             $q$SELECT kind, avg(amount) AS a FROM hv_events GROUP BY 1 HAVING avg(amount) > 4.95$q$),
  ('b HAVING (groups)','on a max',                  $q$SELECT tenant_id, max(event_date) AS d FROM hv_events GROUP BY 1 HAVING max(event_date) > DATE '2026-01-29'$q$),
  ('b HAVING (groups)','on a count of distinct',    $q$SELECT tenant_id, count(DISTINCT counter_a) AS d FROM hv_events GROUP BY 1 HAVING count(DISTINCT counter_a) > 3$q$),
  ('b HAVING (groups)','on a key only (a row condition)', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id > 8$q$),
  ('b HAVING (groups)','on a text key only',        $q$SELECT kind, count(*) AS c FROM hv_events GROUP BY 1 HAVING kind IN ('k1', 'k3')$q$),
  ('b HAVING (groups)','on a date key only',        $q$SELECT event_date, count(*) AS c FROM hv_events GROUP BY 1 HAVING event_date >= DATE '2026-01-25'$q$),
  ('b HAVING (groups)','on the NULL key only',      $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id IS NULL$q$),
  ('b HAVING (groups)','on a key and an aggregate (split)', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id > 8 AND count(*) > 2000$q$),
  ('b HAVING (groups)','on an aggregate that no group passes', $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 99999$q$),
  ('b HAVING (groups)','every group passes',        $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) >= 1$q$),
  ('b HAVING (groups)','one group, no GROUP BY, passes', $q$SELECT count(*) AS c FROM hv_events HAVING count(*) > 100$q$),
  ('b HAVING (groups)','one group, no GROUP BY, fails', $q$SELECT count(*) AS c FROM hv_events HAVING count(*) < 100$q$),
  ('c both',           'a count over the filtered rows', $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE status = 1 GROUP BY 1 HAVING count(*) > 700$q$),
  ('c both',           'a sum over the filtered rows',   $q$SELECT tenant_id, sum(counter_b) AS s FROM hv_events WHERE counter_b > 4 GROUP BY 1 HAVING sum(counter_b) > 5000$q$),
  ('c both',           'a date range and a sum',         $q$SELECT tenant_id, sum(counter_b) AS s FROM hv_events WHERE event_date >= DATE '2026-01-15' GROUP BY 1 HAVING sum(counter_b) > 6000$q$),
  ('c both',           'WHERE on a key, HAVING on a count', $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE tenant_id IN (15, 16, 17) GROUP BY 1 HAVING count(*) > 1$q$),
  ('c both',           'WHERE and HAVING on the same column', $q$SELECT tenant_id, max(counter_b) AS m FROM hv_events WHERE counter_b = 3 GROUP BY 1 HAVING max(counter_b) = 3$q$),
  ('c both',           'HAVING that the WHERE makes impossible', $q$SELECT tenant_id, max(counter_b) AS m FROM hv_events WHERE counter_b < 3 GROUP BY 1 HAVING max(counter_b) > 5$q$),
  ('c both',           'WHERE empties the groups, HAVING on the NULL sum', $q$SELECT tenant_id, sum(counter_a) AS s FROM hv_events WHERE tenant_id = 15 GROUP BY 1 HAVING sum(counter_a) IS NULL$q$),
  ('c both',           'a key in the WHERE and the HAVING', $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE tenant_id > 5 GROUP BY 1 HAVING tenant_id < 12 AND count(*) > 1500$q$),
  ('c both',           'a FILTER, a WHERE and a HAVING',  $q$SELECT tenant_id, count(*) FILTER (WHERE flag_a) AS c FROM hv_events WHERE status < 2 GROUP BY 1 HAVING count(*) FILTER (WHERE flag_a) > 100$q$),
  ('c both',           'a lookup join, WHERE on it, HAVING',  $q$SELECT t.tier, count(*) AS c FROM hv_events e JOIN hv_tenants t ON t.id = e.tenant_id WHERE t.active GROUP BY 1 HAVING count(*) > 5000$q$),
  ('c both',           'a rollup, WHERE and HAVING',      $q$SELECT category_id, status, count(*) AS c FROM hv_events WHERE counter_b > 2 GROUP BY ROLLUP (category_id, status) HAVING count(*) > 1500$q$);
SELECT q.idea, q.name, c.* FROM q, LATERAL having_run(q.sql) c ORDER BY q.ord;

-- part 2: invariants. a_vs_b: the two statements return the same rows or differ (both with the scan on), and as_expected that it is what the pair is for; a_same_off: the first is the same as sift off
CREATE TABLE pairs (ord serial, idea text, name text, a text, b text, expect text DEFAULT 'same');
INSERT INTO pairs (idea, name, a, b) VALUES
  ('HAVING never changes an aggregate', 'the count and the sum of a group that passes',
   $q$SELECT tenant_id, count(*) AS c, sum(counter_a) AS s FROM hv_events GROUP BY 1 HAVING count(*) > 100$q$,
   $q$SELECT * FROM (SELECT tenant_id, count(*) AS c, sum(counter_a) AS s FROM hv_events GROUP BY 1 OFFSET 0) g WHERE c > 100$q$),
  ('HAVING never changes an aggregate', 'a condition on one aggregate of three',
   $q$SELECT kind, count(*) AS c, avg(amount) AS a, max(id) AS m FROM hv_events GROUP BY 1 HAVING max(id) > 29999$q$,
   $q$SELECT * FROM (SELECT kind, count(*) AS c, avg(amount) AS a, max(id) AS m FROM hv_events GROUP BY 1 OFFSET 0) g WHERE m > 29999$q$),
  ('HAVING never changes an aggregate', 'a condition on an aggregate that is not selected',
   $q$SELECT tenant_id, sum(counter_b) AS s FROM hv_events GROUP BY 1 HAVING count(*) > 100$q$,
   $q$SELECT g.tenant_id, g.s FROM (SELECT tenant_id, sum(counter_b) AS s, count(*) AS c FROM hv_events GROUP BY 1 OFFSET 0) g WHERE g.c > 100$q$),
  ('HAVING never changes an aggregate', 'with a WHERE that changes them first',
   $q$SELECT tenant_id, count(*) AS c, sum(counter_b) AS s FROM hv_events WHERE status = 1 GROUP BY 1 HAVING sum(counter_b) > 3000$q$,
   $q$SELECT * FROM (SELECT tenant_id, count(*) AS c, sum(counter_b) AS s FROM hv_events WHERE status = 1 GROUP BY 1 OFFSET 0) g WHERE s > 3000$q$),
  ('HAVING never changes an aggregate', 'one group without GROUP BY',
   $q$SELECT count(*) AS c, sum(counter_a) AS s FROM hv_events HAVING sum(counter_a) > 100$q$,
   $q$SELECT * FROM (SELECT count(*) AS c, sum(counter_a) AS s FROM hv_events OFFSET 0) g WHERE s > 100$q$),
  ('HAVING never changes an aggregate', 'grouping sets',
   $q$SELECT category_id, status, count(*) AS c, sum(counter_a) AS s FROM hv_events GROUP BY CUBE (category_id, status) HAVING count(*) > 2000$q$,
   $q$SELECT * FROM (SELECT category_id, status, count(*) AS c, sum(counter_a) AS s FROM hv_events GROUP BY CUBE (category_id, status) OFFSET 0) g WHERE c > 2000$q$),
  ('WHERE changes the aggregates', 'rows dropped before grouping equal a FILTER on every aggregate and a HAVING on the groups left',
   $q$SELECT tenant_id, count(*) AS c, sum(counter_b) AS s FROM hv_events WHERE status = 1 GROUP BY 1$q$,
   $q$SELECT tenant_id, count(*) FILTER (WHERE status = 1) AS c, sum(counter_b) FILTER (WHERE status = 1) AS s FROM hv_events GROUP BY 1 HAVING count(*) FILTER (WHERE status = 1) > 0$q$),
  ('WHERE changes the aggregates', 'with a HAVING on top of both',
   $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE status = 1 GROUP BY 1 HAVING count(*) > 700$q$,
   $q$SELECT tenant_id, count(*) FILTER (WHERE status = 1) AS c FROM hv_events GROUP BY 1 HAVING count(*) FILTER (WHERE status = 1) > 700$q$),
  ('WHERE changes the aggregates', 'NOT the same as a HAVING on the rows: the count of all the rows against the count of the rows left',
   $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE status = 1 GROUP BY 1 HAVING count(*) > 700$q$,
   $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 700 AND bool_or(status = 1)$q$),
  ('WHERE changes the aggregates', 'a group with no row left does not exist, with a FILTER it has a zero',
   $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE counter_a IS NOT NULL AND tenant_id IN (15, 16) GROUP BY 1$q$,
   $q$SELECT tenant_id, count(*) FILTER (WHERE counter_a IS NOT NULL) AS c FROM hv_events WHERE tenant_id IN (15, 16) GROUP BY 1$q$),
  ('conditions are a set', 'the order of the conditions of a HAVING',
   $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING count(*) > 900 AND sum(counter_a) > 3000 AND tenant_id < 12$q$,
   $q$SELECT tenant_id, count(*) AS c FROM hv_events GROUP BY 1 HAVING tenant_id < 12 AND sum(counter_a) > 3000 AND count(*) > 900$q$),
  ('conditions are a set', 'a HAVING of an AND is a filter of a filter',
   $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE status < 3 GROUP BY 1 HAVING count(*) > 900 AND max(counter_b) = 8$q$,
   $q$SELECT h.tenant_id, h.c FROM (SELECT * FROM (SELECT tenant_id, count(*) AS c, max(counter_b) AS m FROM hv_events WHERE status < 3 GROUP BY 1 OFFSET 0) g WHERE c > 900 OFFSET 0) h WHERE h.m = 8$q$),
  ('conditions are a set', 'a WHERE of two conditions against the same split by a fence (a fence over a plain scan has no source for the scan)',
   $q$SELECT tenant_id, count(*) AS c FROM hv_events WHERE status < 3 AND counter_b > 2 GROUP BY 1$q$,
   $q$SELECT tenant_id, count(*) AS c FROM (SELECT * FROM hv_events WHERE status < 3 OFFSET 0) e WHERE counter_b > 2 GROUP BY 1$q$);
-- the pairs that differ by design: a WHERE counts only the rows left, a FILTER keeps every group
UPDATE pairs SET expect = 'differs' WHERE name LIKE 'NOT the same as a HAVING on the rows%' OR name LIKE 'a group with no row left%';
SELECT p.idea, p.name, c.* FROM pairs p, LATERAL having_same(p.a, p.b, p.expect) c ORDER BY p.ord;

DROP TABLE q, pairs, hv_events, hv_tenants, hv_empty;
DROP FUNCTION having_plan(text), having_rows(text, bool, bool), having_run(text, bool), having_same(text, text, text);
DROP EXTENSION pg_sift;
