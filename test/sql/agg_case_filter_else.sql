-- agg(CASE WHEN p THEN x ELSE k END) with an ELSE other than NULL is planned as aggregates with a FILTER (agg/expr/casefilter.c), for count with any ELSE that is not NULL, and
-- for min, max (integers) and avg (integers and numerics) with ELSE 0: the zeros are values, so a group where p never holds is 0, not NULL, and the zeros count in an average. Each statement runs
-- with the scan and without it and the rows must be the same text (scale of a numeric and sign of a zero included); `rewritten` says that the scan runs the aggregates with no CASE left in them.
-- The shapes where the two forms could differ: groups where p never holds, always holds, or holds only for NULL values; NULL conditions; negative values (min and max against the zero); an empty
-- input; a filter of the aggregate; the types that stay as written (a float, a numeric for min and max, an ELSE that is not a zero of the type), DISTINCT and a value that raises.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE cfe (id int, k int, p text, x float8, n numeric, i int, b bigint, m numeric(10,3), s smallint, t text);
-- k 0..39; p is NULL for a quarter of the rows and never is 'a' in the group k = 7 and always is in k = 8; the values have NULLs and negatives; the group k = 9 has NULL values only where p = 'a'
INSERT INTO cfe SELECT g, g % 40, (array['a', 'b', 'c', NULL])[1 + (g / 40) % 4], CASE WHEN g % 5 = 0 THEN NULL ELSE (g % 17) * 0.25 - 2 END,
       CASE WHEN g % 7 = 0 THEN NULL ELSE ((g % 11) - 3) * 0.5 END, CASE WHEN g % 3 = 0 THEN NULL ELSE g % 9 - 4 END,
       CASE WHEN g % 4 = 0 THEN NULL ELSE (g % 5 - 2) * 3000000000000 + g END, CASE WHEN g % 6 = 0 THEN NULL ELSE (g % 13) * 1.125 - 4 END,
       CASE WHEN g % 8 = 0 THEN NULL ELSE (g % 7 - 3)::smallint END, CASE WHEN g % 9 = 0 THEN NULL ELSE 'v' || g % 5 END
FROM generate_series(1, 6000) g;
UPDATE cfe SET p = 'b' WHERE k = 7 AND p = 'a';
UPDATE cfe SET p = 'a' WHERE k = 8 AND p IS DISTINCT FROM 'a';
UPDATE cfe SET x = NULL, n = NULL, i = NULL, b = NULL, m = NULL, s = NULL WHERE k = 9 AND p = 'a';
UPDATE cfe SET i = abs(i) + 1, b = abs(b) + 1, s = abs(s) + 1 WHERE k = 10;
CREATE INDEX cfe_col ON cfe USING sift_col (id, k, p, x, n, i, b, m, s, t) WITH (key_cut = k);
VACUUM ANALYZE cfe;

CREATE TABLE shapes (n int, label text, q text);
INSERT INTO shapes VALUES
  (1,  'count else 0 int',          $q$SELECT k, count(CASE WHEN p = 'a' THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (2,  'count else 1 null value',   $q$SELECT k, count(CASE WHEN p = 'a' THEN NULL ELSE 1 END) FROM cfe GROUP BY k$q$),
  (3,  'count one else zero',       $q$SELECT k, count(CASE WHEN p = 'a' THEN 1 ELSE 0 END) FROM cfe GROUP BY k$q$),
  (4,  'count float value',         $q$SELECT k, count(CASE WHEN p = 'b' THEN x ELSE 1.5 END) FROM cfe GROUP BY k$q$),
  (5,  'count text else',           $q$SELECT k, count(CASE WHEN p = 'a' THEN t ELSE 'z' END) FROM cfe GROUP BY k$q$),
  (6,  'count condition NULL',      $q$SELECT k, count(CASE WHEN n > 1 THEN i ELSE -1 END) FROM cfe GROUP BY k$q$),
  (7,  'count with a filter',       $q$SELECT k, count(CASE WHEN p = 'a' THEN i ELSE 0 END) FILTER (WHERE x > 0) FROM cfe GROUP BY k$q$),
  (8,  'count ungrouped',           $q$SELECT count(CASE WHEN p = 'a' THEN i ELSE 0 END) a, count(CASE WHEN p = 'c' THEN b ELSE 7 END) c FROM cfe$q$),
  (9,  'count empty input',         $q$SELECT count(CASE WHEN p = 'a' THEN i ELSE 0 END) a, count(CASE WHEN p = 'a' THEN NULL ELSE 1 END) c FROM cfe WHERE k > 100$q$),
  (10, 'avg else 0 int4',           $q$SELECT k, avg(CASE WHEN p = 'a' THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (11, 'avg else 0 int8',           $q$SELECT k, avg(CASE WHEN p = 'b' THEN b ELSE 0 END) FROM cfe GROUP BY k$q$),
  (12, 'avg else 0 int2',           $q$SELECT k, avg(CASE WHEN p = 'c' THEN s ELSE 0 END) FROM cfe GROUP BY k$q$),
  (13, 'avg else 0 numeric',        $q$SELECT k, avg(CASE WHEN p = 'a' THEN n ELSE 0 END) FROM cfe GROUP BY k$q$),
  (14, 'avg else 0 numeric typmod', $q$SELECT k, avg(CASE WHEN p = 'b' THEN m ELSE 0 END) FROM cfe GROUP BY k$q$),
  (15, 'avg condition NULL',        $q$SELECT k, avg(CASE WHEN n > 1 THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (16, 'avg with a filter',         $q$SELECT k, avg(CASE WHEN p = 'a' THEN i ELSE 0 END) FILTER (WHERE id % 3 <> 0) FROM cfe GROUP BY k$q$),
  (17, 'avg ungrouped and empty',   $q$SELECT avg(CASE WHEN p = 'a' THEN i ELSE 0 END) a, avg(CASE WHEN p = 'a' THEN n ELSE 0 END) b FROM cfe$q$),
  (18, 'avg empty input',           $q$SELECT avg(CASE WHEN p = 'a' THEN i ELSE 0 END) a, avg(CASE WHEN p = 'a' THEN n ELSE 0 END) b FROM cfe WHERE k > 100$q$),
  (19, 'avg having',                $q$SELECT k, avg(CASE WHEN p = 'a' THEN i ELSE 0 END) a FROM cfe GROUP BY k HAVING avg(CASE WHEN p = 'a' THEN i ELSE 0 END) > 0.1$q$),
  (20, 'min else 0 int4',           $q$SELECT k, min(CASE WHEN p = 'a' THEN i ELSE 0 END), max(CASE WHEN p = 'a' THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (21, 'min max else 0 int8',       $q$SELECT k, min(CASE WHEN p = 'b' THEN b ELSE 0 END), max(CASE WHEN p = 'b' THEN b ELSE 0 END) FROM cfe GROUP BY k$q$),
  (22, 'min max else 0 int2',       $q$SELECT k, min(CASE WHEN p = 'c' THEN s ELSE 0 END), max(CASE WHEN p = 'c' THEN s ELSE 0 END) FROM cfe GROUP BY k$q$),
  (23, 'min max condition NULL',    $q$SELECT k, min(CASE WHEN n > 1 THEN i ELSE 0 END), max(CASE WHEN n > 1 THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (24, 'min max with a filter',     $q$SELECT k, min(CASE WHEN p = 'a' THEN i ELSE 0 END) FILTER (WHERE id % 2 = 0), max(CASE WHEN p = 'a' THEN i ELSE 0 END) FILTER (WHERE id % 2 = 0) FROM cfe GROUP BY k$q$),
  (25, 'min max ungrouped empty',   $q$SELECT min(CASE WHEN p = 'a' THEN i ELSE 0 END) a, max(CASE WHEN p = 'a' THEN i ELSE 0 END) b FROM cfe WHERE k > 100$q$),
  (26, 'pivot of the three',        $q$SELECT k, avg(CASE WHEN p = 'a' THEN i ELSE 0 END), min(CASE WHEN p = 'a' THEN i ELSE 0 END), count(CASE WHEN p = 'a' THEN i ELSE 0 END), sum(CASE WHEN p = 'a' THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (27, 'avg float stays',           $q$SELECT k, avg(CASE WHEN p = 'a' THEN x ELSE 0 END) FROM cfe GROUP BY k$q$),
  (28, 'min numeric stays',         $q$SELECT k, min(CASE WHEN p = 'a' THEN n ELSE 0 END) FROM cfe GROUP BY k$q$),
  (29, 'max float stays',           $q$SELECT k, max(CASE WHEN p = 'a' THEN x ELSE 0 END) FROM cfe GROUP BY k$q$),
  (30, 'avg else 1 stays',          $q$SELECT k, avg(CASE WHEN p = 'a' THEN i ELSE 1 END) FROM cfe GROUP BY k$q$),
  (31, 'avg else 0.00 stays',       $q$SELECT k, avg(CASE WHEN p = 'a' THEN n ELSE 0.00 END) FROM cfe GROUP BY k$q$),
  (32, 'min else 1 stays',          $q$SELECT k, min(CASE WHEN p = 'a' THEN i ELSE 1 END) FROM cfe GROUP BY k$q$),
  (33, 'avg distinct stays',        $q$SELECT k, avg(DISTINCT CASE WHEN p = 'a' THEN i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (34, 'value guarded by p',        $q$SELECT k, avg(CASE WHEN i <> 0 THEN 100 / i ELSE 0 END), count(CASE WHEN i <> 0 THEN 1 / i ELSE 0 END), max(CASE WHEN i <> 0 THEN 100 / i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (35, 'value that raises',         $q$SELECT k, avg(CASE WHEN p = 'a' THEN 100 / i ELSE 0 END) FROM cfe GROUP BY k$q$),
  (36, 'value that raises in count', $q$SELECT count(CASE WHEN p = 'a' THEN 1 / i ELSE 0 END) FROM cfe$q$);

-- rows: the text of the rows of the scan equals that of the normal plan (scale and sign of zero included); rewritten: no CASE is left in the aggregates the scan runs
SELECT n, label, sift_helpers.agg_check_outcome(q) AS outcome, sift_helpers.plan_pushed_aggregates_have_no_case(q) AS rewritten FROM shapes ORDER BY n;

-- the groups that matter, by value: all ELSE (k = 7), never ELSE (k = 8), NULL values only where the condition holds (k = 9), positive values only (k = 10)
SELECT k, avg(CASE WHEN p = 'a' THEN i ELSE 0 END) a, min(CASE WHEN p = 'a' THEN i ELSE 0 END) mn, max(CASE WHEN p = 'a' THEN i ELSE 0 END) mx,
       count(CASE WHEN p = 'a' THEN i ELSE 0 END) c, count(CASE WHEN p = 'a' THEN NULL ELSE 1 END) cn, avg(CASE WHEN p = 'a' THEN n ELSE 0 END) an
FROM cfe WHERE k IN (7, 8, 9, 10) GROUP BY k ORDER BY k;
SELECT avg(CASE WHEN p = 'a' THEN i ELSE 0 END) a, min(CASE WHEN p = 'a' THEN i ELSE 0 END) mn, count(CASE WHEN p = 'a' THEN NULL ELSE 1 END) cn FROM cfe WHERE k > 100;

DROP TABLE shapes, cfe;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
