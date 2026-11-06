-- A condition that wraps a column in coalesce, nullif, IS [NOT] DISTINCT FROM or an integer widening cast is run as tests of the column (agg/plan_prednorm.c)
-- instead of an expression evaluated for every row. Every statement runs with sift on and off and the rows are compared (md5 of the sorted rows); `native` says the plan
-- lists no generic expression for the filter, `engaged` that the scan ran. The columns have NULLs, so the conditions are tested where a three-valued logic could differ: a NULL
-- column, a NULL constant, a NOT around the condition (which keeps the expression as written), the conditions inside AND and OR, and the aggregate FILTER.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE pn (id int PRIMARY KEY, a float8, i int, s smallint, b bool, t text, g int);
INSERT INTO pn SELECT x, CASE WHEN x % 7 = 0 THEN NULL ELSE (x % 50) / 2.0 END, CASE WHEN x % 11 = 0 THEN NULL ELSE x % 100 END, (x % 30)::smallint,
       CASE WHEN x % 5 = 0 THEN NULL ELSE x % 3 = 0 END, (array['a', 'b', NULL])[1 + x % 3], x % 13 FROM generate_series(1, 6000) x;
CREATE INDEX pn_col ON pn USING sift_col (id, a, i, s, b, t, g) WITH (key_cut = g);
VACUUM ANALYZE pn;

CREATE TABLE shapes (ord serial, name text, cond text);
INSERT INTO shapes (name, cond) VALUES
  ('coalesce > false',        $q$coalesce(a, 0) > 10$q$),
  ('coalesce < true',         $q$coalesce(a, 0) < 5$q$),
  ('coalesce = zero',         $q$coalesce(a, 0) = 0$q$),
  ('coalesce >= negative',    $q$coalesce(a, -1) >= -1$q$),
  ('coalesce <> int',         $q$coalesce(i, 0) <> 3$q$),
  ('coalesce on the right',   $q$10 < coalesce(a, 0)$q$),
  ('coalesce NULL default',   $q$coalesce(a, NULL) > 3$q$),
  ('coalesce stable default', $q$coalesce(a, (current_setting('server_version_num')::int * 0)::float8) < 5$q$),
  ('coalesce under AND',      $q$coalesce(a, 0) < 5 AND coalesce(i, 0) > 50$q$),
  ('coalesce under OR',       $q$coalesce(a, 0) > 10 OR i = 3$q$),
  ('NOT of coalesce',        $q$NOT (coalesce(a, 0) > 10)$q$),
  ('NOT of coalesce true',   $q$NOT (coalesce(a, 0) < 5)$q$),
  ('coalesce NULL arg dropped', $q$coalesce(a, NULL, 0) > 10$q$),
  ('coalesce of an expression', $q$coalesce(a + 1, 0) > 10$q$),
  ('nullif =',                $q$nullif(i, 3) = 4$q$),
  ('nullif >',                $q$nullif(i, 3) > 50$q$),
  ('nullif <> ',              $q$nullif(i, 3) <> 4$q$),
  ('nullif float',            $q$nullif(a, 0) > 1$q$),
  ('nullif hits the value',   $q$nullif(i, 3) = 3$q$),
  ('nullif NULL',             $q$nullif(i, NULL) = 3$q$),
  ('nullif IS NULL',          $q$nullif(i, 3) IS NULL$q$),
  ('distinct',                $q$i IS DISTINCT FROM 3$q$),
  ('not distinct',            $q$i IS NOT DISTINCT FROM 3$q$),
  ('not distinct NULL',       $q$i IS NOT DISTINCT FROM NULL$q$),
  ('distinct NULL',           $q$i IS DISTINCT FROM NULL$q$),
  ('constant first',          $q$3 IS DISTINCT FROM i$q$),
  ('NOT distinct',            $q$NOT (i IS DISTINCT FROM 3)$q$),
  ('NOT not distinct',        $q$NOT (i IS NOT DISTINCT FROM 3)$q$),
  ('distinct under OR',       $q$i IS DISTINCT FROM 3 OR a > 20$q$),
  ('distinct text',           $q$t IS DISTINCT FROM 'a'$q$),
  -- the tests of a boolean column stay lookups of its two values (native is false): the walk of a top-N tests those on the heap row (col_topn_gorder_filter), a comparison it does not
  ('bool column',             $q$b$q$),
  ('bool = true',             $q$b = true$q$),
  ('bool = false',            $q$b = false$q$),
  ('NOT bool',                $q$NOT b$q$),
  ('bool IS TRUE',            $q$b IS TRUE$q$),
  ('bool IS NOT TRUE',        $q$b IS NOT TRUE$q$),
  ('bool IS FALSE',           $q$b IS FALSE$q$),
  ('bool IS NOT FALSE',       $q$b IS NOT FALSE$q$),
  ('bool IS UNKNOWN',         $q$b IS UNKNOWN$q$),
  ('bool IS NOT UNKNOWN',     $q$b IS NOT UNKNOWN$q$),
  ('NOT bool IS TRUE',        $q$NOT (b IS TRUE)$q$),
  ('bool AND',                $q$b AND i > 3$q$),
  ('NOT bool OR',             $q$NOT b OR i > 90$q$),
  ('widening =',              $q$i::bigint = 3$q$),
  ('widening <',              $q$i::bigint < 50$q$),
  ('widening smallint',       $q$s::int >= 10$q$),
  ('widening smallint bigint', $q$s::bigint <> 3$q$),
  ('widening on the right',   $q$50 > s::int$q$),
  ('widening beyond int4',    $q$i::bigint < 5000000000$q$),
  ('widening NULL constant',  $q$i::bigint = NULL$q$),
  ('float cast stays',        $q$i::float8 > 50.5$q$);

-- native: no generic expression among the filters; engaged: the scan ran; same: the rows of the plan without the scan
CREATE FUNCTION shape_check(q text) RETURNS TABLE (native bool, engaged bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  native := engaged AND plan LIKE '%Filters:%' AND plan !~ 'Filters: [a-z_]+: ';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
-- grouped by the key and ungrouped; the FILTER of an aggregate takes the same condition
SELECT s.name, c.* FROM shapes s, LATERAL shape_check('SELECT g, count(*), sum(id) FROM pn WHERE ' || s.cond || ' GROUP BY g') c ORDER BY s.ord;
SELECT s.name, c.* FROM shapes s, LATERAL shape_check('SELECT count(*), sum(i) FROM pn WHERE ' || s.cond) c ORDER BY s.ord;
SELECT s.name, c.same FROM shapes s, LATERAL shape_check('SELECT g, count(*) FILTER (WHERE ' || s.cond || '), sum(i) FILTER (WHERE ' || s.cond || ') FROM pn GROUP BY g') c WHERE NOT c.same ORDER BY s.ord;

-- the rows by value
SELECT count(*) FROM pn WHERE coalesce(a, 0) < 5;
SELECT count(*) FROM pn WHERE coalesce(a, 0) > 10;
SELECT count(*) FROM pn WHERE nullif(i, 3) = 4;
SELECT count(*) FROM pn WHERE i IS DISTINCT FROM 3;
SELECT count(*) FROM pn WHERE b IS NOT TRUE;
SELECT count(*) FROM pn WHERE i::bigint = 3;

DROP FUNCTION shape_check(text);
DROP TABLE shapes, pn;
DROP EXTENSION pg_sift;
