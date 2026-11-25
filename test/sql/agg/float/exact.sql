-- The pushed float aggregates give PostgreSQL's bits, not only its values: random inputs, many fractions, every group size, each query run with sift on and off and
-- the rows compared as text (the shortest exact float8 output). `engaged` counts the queries whose plan runs in the scan. A server built with fused multiply-add
-- (aarch64, or x86-64 for a newer CPU) rounds `lo + p * (hi - lo)` and `p * (n - 1) - first` once, one built without it twice, and its function for an array of
-- fractions may differ from the scalar one: the scan finds out how this server does each (agg/osa/round.c). A sum next to a percentile is not compared: the normal
-- plan sorts the rows of the group by the percentile's argument, and the order of the rows is that of the sum.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SELECT setseed(0.4242);
CREATE TABLE fe_t (id int PRIMARY KEY, g int NOT NULL, x float8, y float8, v numeric(12, 4));
INSERT INTO fe_t SELECT n, floor(31 * random() ^ 2)::int,
  CASE WHEN random() < 0.05 THEN NULL ELSE (random() - 0.3) * 10 ^ floor(random() * 7) END,
  CASE WHEN random() < 0.05 THEN NULL ELSE (random() - 0.5) * 1000 + n / 7.0 END,
  CASE WHEN random() < 0.05 THEN NULL ELSE round((random() * 100000)::numeric, 4) END
  FROM generate_series(1, 4000) n;
CREATE INDEX fe_t_col ON fe_t USING sift_col (id, g, x, y, v);
VACUUM (FREEZE, ANALYZE) fe_t;

CREATE TABLE fe_frac AS SELECT n, CASE WHEN n % 3 = 0 THEN round(random()::numeric, 3)::float8 ELSE random() END AS f FROM generate_series(1, 48) n;
CREATE TABLE q (ord serial, kind text, sql text);
INSERT INTO q (kind, sql)
  SELECT 'percentile_cont', format('SELECT g, percentile_cont(%L::float8) WITHIN GROUP (ORDER BY x) FROM fe_t GROUP BY g', f) FROM fe_frac ORDER BY n;
INSERT INTO q (kind, sql)
  SELECT 'percentile_cont desc', format('SELECT g, percentile_cont(%L::float8) WITHIN GROUP (ORDER BY y DESC) FROM fe_t GROUP BY g', f) FROM fe_frac ORDER BY n;
INSERT INTO q (kind, sql)
  SELECT 'percentile_cont array', format('SELECT g, percentile_cont(array[%L, %L, %L]::float8[]) WITHIN GROUP (ORDER BY x) FROM fe_t GROUP BY g', f, 1 - f, f / 2) FROM fe_frac ORDER BY n;
INSERT INTO q (kind, sql)
  SELECT 'percentile_cont beside', format('SELECT g, percentile_cont(%L::float8) WITHIN GROUP (ORDER BY x), percentile_cont(%L::float8) WITHIN GROUP (ORDER BY y), count(*) FROM fe_t GROUP BY g', f, 1 - f)
  FROM fe_frac WHERE n % 4 = 0 ORDER BY n;
INSERT INTO q (kind, sql)
  SELECT 'percentile_disc', format('SELECT g, percentile_disc(%L::float8) WITHIN GROUP (ORDER BY x), percentile_disc(%L::float8) WITHIN GROUP (ORDER BY v) FROM fe_t GROUP BY g', f, f)
  FROM fe_frac WHERE n % 4 = 1 ORDER BY n;
INSERT INTO q (kind, sql) VALUES
  ('median',      'SELECT g, percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM fe_t GROUP BY g'),
  ('no GROUP BY', 'SELECT percentile_cont(0.37) WITHIN GROUP (ORDER BY x), percentile_cont(array[0.11, 0.97]) WITHIN GROUP (ORDER BY y) FROM fe_t'),
  ('avg sum',     'SELECT g, avg(x), sum(x), avg(y), sum(y), avg(v), sum(v) FROM fe_t GROUP BY g'),
  ('variance',    'SELECT g, var_pop(x), var_samp(x), variance(y), stddev(x), stddev_pop(y), stddev_samp(y), stddev(v), variance(v) FROM fe_t GROUP BY g'),
  ('covariance',  'SELECT g, corr(x, y), covar_pop(x, y), covar_samp(y, x), corr(y, x) FROM fe_t GROUP BY g'),
  ('regression',  'SELECT g, regr_slope(y, x), regr_intercept(y, x), regr_r2(y, x), regr_avgx(y, x), regr_avgy(y, x), regr_count(y, x) FROM fe_t GROUP BY g'),
  ('regression sums', 'SELECT g, regr_sxx(y, x), regr_syy(y, x), regr_sxy(y, x) FROM fe_t GROUP BY g'),
  ('no GROUP BY stats', 'SELECT avg(x), sum(y), stddev(x), corr(x, y), regr_slope(y, x) FROM fe_t'),
  ('filtered',    'SELECT g, stddev(x) FILTER (WHERE id % 3 = 0), avg(y) FILTER (WHERE id % 5 = 0) FROM fe_t GROUP BY g'),
  ('filtered percentile', 'SELECT g, percentile_cont(0.61) WITHIN GROUP (ORDER BY x) FILTER (WHERE id % 3 = 0), count(*) FROM fe_t GROUP BY g');

CREATE FUNCTION fe_check(q text) RETURNS TABLE (engaged bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg%';
  FOR i IN 1..2 LOOP
    PERFORM set_config('sift.enable_agg', CASE i WHEN 1 THEN 'on' ELSE 'off' END, true);
    EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
    IF i = 1 THEN b := a; END IF;
  END LOOP;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
CREATE TABLE fe_res AS SELECT q.ord, q.kind, q.sql, c.* FROM q, LATERAL fe_check(q.sql) c;
SELECT kind, count(*) AS queries, count(*) FILTER (WHERE engaged) AS engaged, count(*) FILTER (WHERE same) AS same FROM fe_res GROUP BY kind ORDER BY min(ord);
SELECT ord, kind, sql FROM fe_res WHERE NOT same ORDER BY ord LIMIT 5;

DROP FUNCTION fe_check(text);
DROP TABLE fe_res, q, fe_frac, fe_t;
DROP EXTENSION pg_sift;
