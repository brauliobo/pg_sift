-- array_agg(DISTINCT x ORDER BY x) (and string_agg) with the ORDER BY the DISTINCT argument in its own ascending order, NULLs last: the distinct values of a group are the set the scan keeps and sorts, not every
-- row in a tuplesort. Any other order (DESC, NULLS FIRST), a FILTER, an expression, a second DISTINCT aggregate and the ones beside it give the rows of sift off.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE da (id int PRIMARY KEY, g int NOT NULL, k int, s text, d date, n numeric(10, 2), b bool);
INSERT INTO da SELECT i, 1 + i % 6, CASE WHEN i % 9 = 0 THEN NULL ELSE (i * 7) % 23 END, CASE WHEN i % 11 = 0 THEN NULL ELSE 'v' || (i * 5) % 17 END,
  CASE WHEN i % 13 = 0 THEN NULL ELSE date '2024-01-01' + (i * 3) % 30 END, ((i * 37) % 400) / 4.0, i % 3 = 0 FROM generate_series(1, 30000) i;
CREATE INDEX da_col ON da USING sift_col (id, g, k, s, d, n, b);
VACUUM (FREEZE, ANALYZE) da;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('int',                      $q$SELECT g, array_agg(DISTINCT k ORDER BY k) FROM da GROUP BY g$q$),
  ('text',                     $q$SELECT g, array_agg(DISTINCT s ORDER BY s) FROM da GROUP BY g$q$),
  ('date',                     $q$SELECT g, array_agg(DISTINCT d ORDER BY d) FROM da GROUP BY g$q$),
  ('numeric',                  $q$SELECT g, array_agg(DISTINCT n ORDER BY n) FROM da GROUP BY g$q$),
  ('string_agg',               $q$SELECT g, string_agg(DISTINCT s, ',' ORDER BY s) FROM da GROUP BY g$q$),
  ('no GROUP BY',              $q$SELECT array_agg(DISTINCT k ORDER BY k), array_agg(DISTINCT b ORDER BY b) FROM da$q$),
  ('no row',                   $q$SELECT array_agg(DISTINCT k ORDER BY k) FROM da WHERE id < 0$q$),
  ('all NULL group',           $q$SELECT g, array_agg(DISTINCT k ORDER BY k) FROM da WHERE k IS NULL GROUP BY g$q$),
  ('two aggregates',           $q$SELECT g, array_agg(DISTINCT k ORDER BY k), array_agg(DISTINCT s ORDER BY s), count(DISTINCT k), count(*) FROM da GROUP BY g$q$),
  ('descending',               $q$SELECT g, array_agg(DISTINCT k ORDER BY k DESC) FROM da GROUP BY g$q$),
  ('NULLS FIRST',              $q$SELECT g, array_agg(DISTINCT k ORDER BY k NULLS FIRST) FROM da GROUP BY g$q$),
  ('with a filter',            $q$SELECT g, array_agg(DISTINCT k ORDER BY k) FILTER (WHERE id % 4 = 0) FROM da GROUP BY g$q$),
  ('an expression',            $q$SELECT g, array_agg(DISTINCT k % 5 ORDER BY k % 5) FROM da GROUP BY g$q$),
  ('with a WHERE and a limit', $q$SELECT g, array_agg(DISTINCT k ORDER BY k) FROM da WHERE id > 5000 GROUP BY g ORDER BY g LIMIT 3$q$);
CREATE FUNCTION do_check(q text) RETURNS TABLE (engaged bool, same bool) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.plan_engaged_and_answers_same(q) $$;
SELECT q.name, c.* FROM q, LATERAL do_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION do_check(text);
DROP TABLE q, da;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
