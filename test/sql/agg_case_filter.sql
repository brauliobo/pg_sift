-- agg(CASE WHEN p THEN x [ELSE NULL | 0] END) is planned as agg(x) FILTER (WHERE p) (agg/expr/casefilter.c): the aggregate runs on the kernels of x under a test of p. Every
-- statement runs with sift on and off and the rows are compared (md5 of the sorted rows), the plan says whether the CASE was rewritten (no CASE left in the pushed aggregates)
-- and whether the scan ran. The shapes are the ones where the two forms could differ: a group where p never holds, NULL values and NULL conditions, an empty input, ELSE 0
-- (a sum of zeros is not NULL), a filter of the aggregate, a simple CASE, and a value that raises where p does not hold.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE cf (id int, k int, p text, x float8, n numeric, i int, b bigint, m numeric(10,3));
-- k 0..39; p is NULL for a quarter of the rows, varies inside every group, and never is 'a' in the groups k = 7 and 8; x, n and i have NULLs; the group k = 9 has
-- NULL values only where p = 'a'
INSERT INTO cf SELECT g, g % 40, (array['a', 'b', 'c', NULL])[1 + (g / 40) % 4], CASE WHEN g % 5 = 0 THEN NULL ELSE (g % 17) * 0.25 END,
       CASE WHEN g % 7 = 0 THEN NULL ELSE (g % 11) * 0.5 END, CASE WHEN g % 3 = 0 THEN NULL ELSE g % 9 END, g::bigint * 1000000000, CASE WHEN g % 6 = 0 THEN NULL ELSE (g % 13) * 1.125 END
FROM generate_series(1, 6000) g;
UPDATE cf SET p = 'b' WHERE k = 7 AND p = 'a';
UPDATE cf SET p = 'c' WHERE k = 8 AND p = 'a';
UPDATE cf SET x = NULL, n = NULL, i = NULL WHERE k = 9 AND p = 'a';
CREATE INDEX cf_col ON cf USING sift_col (id, k, p, x, n, i, b, m) WITH (key_cut = k);
VACUUM ANALYZE cf;

CREATE TABLE shapes (ord serial, name text, q text);
INSERT INTO shapes (name, q) VALUES
  ('sum else null',          $q$SELECT k, sum(CASE WHEN p = 'a' THEN x END) FROM cf GROUP BY k$q$),
  ('sum else null spelled',  $q$SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE NULL END) FROM cf GROUP BY k$q$),
  ('sum else 0 float',       $q$SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) FROM cf GROUP BY k$q$),
  ('sum else 0 numeric',     $q$SELECT k, sum(CASE WHEN p = 'b' THEN n ELSE 0 END) FROM cf GROUP BY k$q$),
  ('sum else 0.00 stays',     $q$SELECT k, sum(CASE WHEN p = 'b' THEN n ELSE 0.00 END) FROM cf GROUP BY k$q$),
  ('sum else 0 typmod',     $q$SELECT k, sum(CASE WHEN p = 'b' THEN m ELSE 0 END) FROM cf GROUP BY k$q$),
  ('sum else 0 int',         $q$SELECT k, sum(CASE WHEN p = 'c' THEN i ELSE 0 END) FROM cf GROUP BY k$q$),
  ('sum else 0 bigint',      $q$SELECT k, sum(CASE WHEN p = 'c' THEN b ELSE 0 END) FROM cf GROUP BY k$q$),
  ('sum one else 0',         $q$SELECT k, sum(CASE WHEN p = 'a' THEN 1 ELSE 0 END) FROM cf GROUP BY k$q$),
  ('condition NULL',         $q$SELECT k, sum(CASE WHEN n > 3 THEN x ELSE 0 END) FROM cf GROUP BY k$q$),
  ('count one',              $q$SELECT k, count(CASE WHEN p = 'a' THEN 1 END) FROM cf GROUP BY k$q$),
  ('count value',            $q$SELECT k, count(CASE WHEN p = 'a' THEN x END) FROM cf GROUP BY k$q$),
  ('avg',                    $q$SELECT k, avg(CASE WHEN p = 'a' THEN x END) FROM cf GROUP BY k$q$),
  ('min max',                $q$SELECT k, min(CASE WHEN p = 'b' THEN i END), max(CASE WHEN p = 'b' THEN n END) FROM cf GROUP BY k$q$),
  ('avg else 0 stays',       $q$SELECT k, avg(CASE WHEN p = 'a' THEN x ELSE 0 END) FROM cf GROUP BY k$q$),
  ('min else 0 stays',       $q$SELECT k, min(CASE WHEN p = 'a' THEN x ELSE 0 END) FROM cf GROUP BY k$q$),
  ('aggregate filter',       $q$SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) FILTER (WHERE i > 3) FROM cf GROUP BY k$q$),
  ('simple case',            $q$SELECT k, sum(CASE p WHEN 'b' THEN i ELSE 0 END) FROM cf GROUP BY k$q$),
  ('pivot',                  $q$SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) a, sum(CASE WHEN p = 'b' THEN x ELSE 0 END) b, count(*) FROM cf WHERE k < 20 GROUP BY k$q$),
  ('ungrouped',              $q$SELECT sum(CASE WHEN p = 'a' THEN x ELSE 0 END), sum(CASE WHEN p = 'a' THEN x END), count(CASE WHEN p = 'a' THEN 1 END) FROM cf$q$),
  ('empty input',            $q$SELECT sum(CASE WHEN p = 'a' THEN x ELSE 0 END), sum(CASE WHEN p = 'a' THEN x END), count(CASE WHEN p = 'a' THEN 1 END) FROM cf WHERE k > 100$q$),
  ('having',                 $q$SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) s FROM cf GROUP BY k HAVING sum(CASE WHEN p = 'a' THEN x ELSE 0 END) > 100$q$),
  ('sub-select',             $q$SELECT k, s FROM (SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) s FROM cf GROUP BY k) t WHERE s IS NOT NULL$q$),
  ('value that raises',      $q$SELECT k, sum(CASE WHEN x <> 0 THEN 1 / x END), sum(CASE WHEN x <> 0 THEN 1 / x ELSE 0 END) FROM cf GROUP BY k$q$),
  ('distinct stays',         $q$SELECT k, sum(DISTINCT CASE WHEN p = 'a' THEN i END) FROM cf GROUP BY k$q$),
  ('two branches stay',      $q$SELECT k, sum(CASE WHEN p = 'a' THEN i WHEN p = 'b' THEN 2 * i END) FROM cf GROUP BY k$q$),
  ('sub-select value stays', $q$SELECT k, sum(CASE WHEN p = 'a' THEN (SELECT 1) END) FROM cf GROUP BY k$q$);

-- rewritten: the CASE is gone from the pushed aggregates (the line of the plan that lists them); engaged: the scan ran; same: the rows of the plan without the scan
CREATE FUNCTION shape_check(q text) RETURNS TABLE (rewritten bool, engaged bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  rewritten := engaged AND plan LIKE '%Pushed Aggregates:%' AND plan !~ 'Pushed Aggregates:[^\n]*CASE';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT s.name, c.* FROM shapes s, LATERAL shape_check(s.q) c ORDER BY s.ord;

-- the empty groups and the NULL cases by value
SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) s0, sum(CASE WHEN p = 'a' THEN x END) sn FROM cf WHERE k IN (7, 8, 9, 10) GROUP BY k ORDER BY k;
SELECT sum(CASE WHEN p = 'a' THEN x ELSE 0 END) s0, sum(CASE WHEN p = 'a' THEN x END) sn, count(CASE WHEN p = 'a' THEN 1 END) c FROM cf WHERE k > 100;

-- a statement the scan cannot run keeps the plan of the CASE as written (same rows, no rewrite)
SET sift.enable_agg = off;
SELECT k, sum(CASE WHEN p = 'a' THEN x ELSE 0 END) FROM cf WHERE k = 9 GROUP BY k;
RESET sift.enable_agg;

DROP FUNCTION shape_check(text);
DROP TABLE shapes, cf;
DROP EXTENSION pg_sift;
