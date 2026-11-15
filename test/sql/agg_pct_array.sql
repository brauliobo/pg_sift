-- percentile_cont(array[..]) and percentile_disc(array[..]) WITHIN GROUP of a constant array of fractions: the scan keeps the values once and picks the ranks of every fraction (the array of the
-- percentile of each fraction); the normal plan sorts the values of every group in PostgreSQL's own transition function. `engaged` says the plan runs in the scan, `pushed` that the scan picks the percentiles itself, `same` that the rows (or the error)
-- are those of sift off. A NULL fraction is a NULL element, a group with no value a NULL, the result has the dimension and lower bound of the fractions; fractions outside [0, 1] and an array
-- that does not start at 1 stay in the normal plan, which raises its errors.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE pa_t (id int PRIMARY KEY, g int NOT NULL, x float8, i int, d date, t timestamptz);
INSERT INTO pa_t SELECT n, 1 + n % 5, CASE WHEN n % 5 = 3 OR n % 7 = 0 THEN NULL ELSE ((n * 37) % 1000) / 10.0 END, CASE WHEN n % 11 = 0 THEN NULL ELSE (n * 13) % 200 END,
  date '2024-01-01' + (n * 7) % 400, timestamptz '2024-01-01' + n * interval '97 minutes' FROM generate_series(1, 30000) n;
CREATE INDEX pa_t_col ON pa_t USING sift_col (id, g, x, i, d, t);
VACUUM (FREEZE, ANALYZE) pa_t;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('quartiles',                 $q$SELECT g, percentile_cont(array[0.25, 0.5, 0.75]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('out of order, repeated',    $q$SELECT g, percentile_cont(array[0.9, 0.1, 0.5, 0.5, 0, 1]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('a NULL fraction',           $q$SELECT g, percentile_cont(array[0.75, NULL, 0.25]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('only NULL fractions',       $q$SELECT g, percentile_cont(array[NULL::float8]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('no fraction',               $q$SELECT g, percentile_cont('{}'::float8[]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('descending',                $q$SELECT g, percentile_cont(array[0.1, 0.9]) WITHIN GROUP (ORDER BY x DESC) FROM pa_t GROUP BY g$q$),
  ('disc of integers',          $q$SELECT g, percentile_disc(array[0.1, 0.5, 0.99]) WITHIN GROUP (ORDER BY i) FROM pa_t GROUP BY g$q$),
  ('disc of dates',             $q$SELECT g, percentile_disc(array[0.25, 0.75]) WITHIN GROUP (ORDER BY d) FROM pa_t GROUP BY g$q$),
  ('disc of timestamps, DESC',  $q$SELECT g, percentile_disc(array[0.25, 0.75]) WITHIN GROUP (ORDER BY t DESC) FROM pa_t GROUP BY g$q$),
  ('disc of floats',            $q$SELECT g, percentile_disc(array[0, 0.3, 1]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('folded fractions',          $q$SELECT g, percentile_cont(array[1.0 / 4, 0.5::float8, 3 * 0.25]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('no GROUP BY',               $q$SELECT percentile_cont(array[0.25, 0.75]) WITHIN GROUP (ORDER BY x), count(*) FROM pa_t$q$),
  ('no row',                    $q$SELECT percentile_cont(array[0.25, 0.75]) WITHIN GROUP (ORDER BY x) FROM pa_t WHERE id < 0$q$),
  ('an element',                $q$SELECT g, (percentile_cont(array[0.25, 0.75]) WITHIN GROUP (ORDER BY x))[2] FROM pa_t GROUP BY g$q$),
  ('HAVING',                    $q$SELECT g FROM pa_t GROUP BY g HAVING percentile_cont(array[0.5]) WITHIN GROUP (ORDER BY x) IS NOT NULL$q$),
  ('beside scalar percentiles', $q$SELECT g, percentile_cont(array[0.25, 0.75]) WITHIN GROUP (ORDER BY x), percentile_cont(0.5) WITHIN GROUP (ORDER BY x), count(x) FROM pa_t GROUP BY g$q$),
  ('two arrays on two columns', $q$SELECT g, percentile_cont(array[0.25, 0.75]) WITHIN GROUP (ORDER BY x), percentile_disc(array[0.25, 0.75]) WITHIN GROUP (ORDER BY i) FROM pa_t GROUP BY g$q$),
  ('with a filter',             $q$SELECT g, percentile_cont(array[0.25, 0.75]) WITHIN GROUP (ORDER BY x) FILTER (WHERE id > 100) FROM pa_t GROUP BY g$q$),
  ('a fraction above 1',        $q$SELECT g, percentile_cont(array[0.5, 1.5]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('a lower bound of 2',        $q$SELECT g, percentile_cont('[2:3]={0.5,0.25}'::float8[]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$),
  ('a two-dimensional array',   $q$SELECT g, percentile_cont('{{0.5,0.25},{0.1,0.2}}'::float8[]) WITHIN GROUP (ORDER BY x) FROM pa_t GROUP BY g$q$);
CREATE FUNCTION pct_check(q text) RETURNS TABLE (engaged bool, pushed bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  pushed := plan ~ 'Pushed Aggregates:[^\n]*percentile_(cont|disc)\([0-9.]+\) WITHIN GROUP';
  FOR i IN 1..2 LOOP
    PERFORM set_config('sift.enable_agg', CASE i WHEN 1 THEN 'on' ELSE 'off' END, true);
    BEGIN
      EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
    EXCEPTION WHEN OTHERS THEN a := 'error: ' || SQLERRM;
    END;
    IF i = 1 THEN b := a; END IF;
  END LOOP;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT q.name, c.* FROM q, LATERAL pct_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION pct_check(text);
DROP TABLE q, pa_t;
DROP EXTENSION pg_sift;
