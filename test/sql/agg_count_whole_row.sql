-- count(e) and count(e.*) of the whole row of the scanned table. The row datum of a tuple is never NULL, even when every field is (count(e) counts a row of NULL fields, while
-- `e IS NULL` is true for it), so count(e) is count(*) as long as no outer join nulls the table (its rows are then missing) or preserves rows of another table that have no row of it.
-- Those two cases keep the normal plan. With the table scans off, `scan same` says the scan node runs and its rows are those of sift off, `plain same` that the normal plan
-- runs (and gives the same rows).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE cw (grp int, a int, b int) WITH (autovacuum_enabled = false);
CREATE TABLE cw_empty (grp int, a int) WITH (autovacuum_enabled = false);
CREATE TABLE lk (id int PRIMARY KEY, name text);
-- groups 1-3 with values, group 4 with NULL values only, rows with a NULL key, three rows of all NULL fields (whose row is not NULL for count(e))
INSERT INTO cw SELECT 1 + i % 3, i % 5, CASE WHEN i % 4 = 0 THEN NULL ELSE i % 7 END FROM generate_series(1, 600) i;
INSERT INTO cw SELECT 4, NULL, NULL FROM generate_series(1, 2);
INSERT INTO cw SELECT NULL, i, i FROM generate_series(1, 5) i;
INSERT INTO cw SELECT NULL, NULL, NULL FROM generate_series(1, 3);
INSERT INTO lk VALUES (1, 'one'), (2, 'two'), (3, 'three'), (5, 'five');
CREATE INDEX cw_col ON cw USING sift_col (grp, a, b);
CREATE INDEX cw_empty_col ON cw_empty USING sift_col (grp, a);
VACUUM (FREEZE, ANALYZE) cw, cw_empty, lk;

CREATE TABLE cases (n serial, label text, q text);
INSERT INTO cases (label, q) VALUES
  ('whole row: count(e) and count(*)', $q$SELECT count(e) AS e, count(*) AS s FROM cw e$q$),
  ('whole row: count(e.*)', $q$SELECT count(e.*) AS e FROM cw e$q$),
  ('whole row: rows of NULL fields are counted', $q$SELECT count(e) AS e, count(e.a) AS a, count(*) FILTER (WHERE e.a IS NULL AND e.b IS NULL) AS nulls FROM cw e$q$),
  ('whole row: grouped, a NULL key and NULL fields', $q$SELECT grp, count(e) AS e, count(*) AS s, count(a) AS a FROM cw e GROUP BY 1$q$),
  ('whole row: a name for the table', $q$SELECT grp, count(x) AS c FROM cw x GROUP BY 1$q$),
  ('whole row: with a WHERE', $q$SELECT grp, count(e) AS c FROM cw e WHERE a > 2 GROUP BY 1$q$),
  ('whole row: with a FILTER', $q$SELECT grp, count(e) FILTER (WHERE a > 2) AS c, count(e) AS n FROM cw e GROUP BY 1$q$),
  ('whole row: in a HAVING', $q$SELECT grp FROM cw e GROUP BY 1 HAVING count(e) > 100$q$),
  ('whole row: in a HAVING beside a sum', $q$SELECT grp, sum(a) AS s FROM cw e GROUP BY 1 HAVING count(e) > 5 AND sum(a) IS NOT NULL$q$),
  ('whole row: a WHERE that drops every row, no GROUP BY', $q$SELECT count(e) AS c FROM cw e WHERE a > 100$q$),
  ('whole row: a WHERE that drops every row, grouped', $q$SELECT grp, count(e) AS c FROM cw e WHERE a > 100 GROUP BY 1$q$),
  ('whole row: an empty table', $q$SELECT count(e) AS c FROM cw_empty e$q$),
  ('whole row: an empty table, grouped', $q$SELECT grp, count(e) AS c FROM cw_empty e GROUP BY 1$q$),
  ('whole row: ROLLUP', $q$SELECT grp, count(e) AS c FROM cw e GROUP BY ROLLUP (grp)$q$),
  ('whole row: top-N', $q$SELECT grp, count(e) AS c FROM cw e GROUP BY 1 ORDER BY c DESC, 1 LIMIT 2$q$),
  ('whole row: DISTINCT stays as it was', $q$SELECT grp, count(DISTINCT e) AS c FROM cw e GROUP BY 1$q$),
  ('join: inner join to a lookup', $q$SELECT l.name, count(e) AS c FROM cw e JOIN lk l ON l.id = e.grp GROUP BY 1$q$),
  ('join: the scanned table preserved by a LEFT JOIN', $q$SELECT e.grp, count(e) AS c, count(*) AS s FROM cw e LEFT JOIN lk l ON l.id = e.grp GROUP BY 1$q$),
  ('join: the preserved table and the nullable lookup row', $q$SELECT e.grp, count(e) AS c, count(l) AS l FROM cw e LEFT JOIN lk l ON l.id = e.grp GROUP BY 1$q$),
  ('join: the scanned table is the nullable side (rows missing)', $q$SELECT l.id, count(e) AS c, count(*) AS s FROM lk l LEFT JOIN cw e ON e.grp = l.id GROUP BY 1$q$),
  ('join: a RIGHT JOIN that preserves the lookup', $q$SELECT l.id, count(e) AS c, count(*) AS s FROM cw e RIGHT JOIN lk l ON l.id = e.grp GROUP BY 1$q$),
  ('join: a FULL JOIN', $q$SELECT l.id, e.grp, count(e) AS c, count(*) AS s FROM cw e FULL JOIN lk l ON l.id = e.grp GROUP BY 1, 2$q$),
  ('join: the lookup row, not the scanned table', $q$SELECT e.grp, count(l) AS c FROM cw e LEFT JOIN lk l ON l.id = e.grp GROUP BY 1$q$),
  ('system column: max(ctid) keeps the normal plan', $q$SELECT grp, max(ctid) AS m FROM cw e GROUP BY 1$q$);
-- This spec pins the count(e) of the normal plan. With the table scans off, the child-first plan of a LEFT JOIN (sift.agg_child_first, covered in agg_child_first_gate)
-- wins on PostgreSQL 17, whose estimates carry the disabled scans as a cost, and not on 18, where they are a count: the spec does not depend on the major.
SET sift.agg_child_first = off;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
SELECT n, label, sift_helpers.agg_check_outcome(q) FROM cases ORDER BY n;
RESET sift.agg_child_first;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_indexonlyscan;

DROP TABLE cases, cw, cw_empty, lk;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
