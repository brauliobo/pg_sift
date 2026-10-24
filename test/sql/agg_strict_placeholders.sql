-- An aggregate's argument or FILTER over a column of a query joined from the left (a spine of periods LEFT JOIN the parsed rows, under grouping
-- sets) reaches the scan as a placeholder. A placeholder that stands for a strict expression of the scanned table's columns is that expression,
-- NULL where the join finds no row, so the cast of a map value is the float leaf: avg is the leaf aggregate and the FILTER a comparison of the
-- decoded values. A constant, a non-strict expression (coalesce, CASE) or a volatile one keeps the placeholder: it is not NULL where the join
-- finds no row (or not known to be), and the results must stay those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, day date NOT NULL, metrics_a jsonb);
INSERT INTO facts
SELECT i, i % 37, date '2026-01-01' + i % 400,
       CASE WHEN i % 23 = 0 THEN NULL
            ELSE jsonb_build_object('mhead', i % 10 + 1, 'service_staff', CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7) % 10 + 1 END) END
FROM generate_series(1, 8000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, day, metrics_a) WITH (paths = 'metrics_a.* float8');
VACUUM (FREEZE, ANALYZE) facts;
\ir test/agg_check_query.sql

-- the report over a spine (the period 'none' matches no row) with `value` defined as `expr`; aggs reads `value`
CREATE FUNCTION report(expr text, aggs text) RETURNS text LANGUAGE sql AS $f$
SELECT format($q$
 WITH periods(period, range) AS (VALUES ('current', daterange('2026-07-01', '2027-02-04', '[]')), ('previous', daterange('2026-01-01', '2026-06-30', '[]')), ('none', daterange('2030-01-01', '2030-12-31', '[]'))),
      parsed AS (SELECT f.id, f.day, r.key, %s AS value FROM facts f, jsonb_each(f.metrics_a) r)
 SELECT p.period, key, %s FROM periods p LEFT JOIN parsed x ON x.day <@ p.range GROUP BY GROUPING SETS ((p.period, key), (p.period)) $q$, expr, aggs)
$f$;
CREATE FUNCTION pushed(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP
    IF l LIKE '%Pushed Aggregates:%' THEN RETURN btrim(l); END IF;
  END LOOP;
  RETURN 'no SiftAgg';
END $$;

CREATE TABLE cases (name text, expr text);
INSERT INTO cases VALUES
  ('strict cast', $$(r.value #>> '{}')::float8$$),
  ('strict arithmetic', $$(r.value #>> '{}')::float8 * 2 + 1$$),
  ('constant', $$1::float8$$),
  ('coalesce', $$coalesce((r.value #>> '{}')::float8, 0)$$),
  ('case', $$CASE WHEN r.value IS NULL THEN -1 ELSE (r.value #>> '{}')::float8 END$$),
  ('volatile', $$(r.value #>> '{}')::float8 + floor(random() * 0)$$);

-- the strict expressions are the leaf (avg(scores.*.value)); the others keep the expression or are not pushed
SELECT name, pushed(report(expr, 'avg(value), count(*) FILTER (WHERE value >= 8) AS high, count(DISTINCT id) FILTER (WHERE value <= 6) AS low'))
FROM cases ORDER BY name;

-- every case answers as the normal plan, the period that matches nothing included
SELECT name, c.*
FROM cases, check_query(report(expr, 'round(avg(value)::numeric, 6) AS value, count(*) FILTER (WHERE value >= 8) AS high, count(DISTINCT id) FILTER (WHERE value <= 6) AS low, sum(value) AS total')) c
ORDER BY name;

-- the rows of the period that matches nothing, with the scan: NULL aggregates of the value for the strict expressions; the constant and the
-- non-strict expressions keep the placeholder, so they are NULL there too (not 1, not 0, not -1)
CREATE FUNCTION none_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  EXECUTE 'SELECT string_agg(row(t.*)::text, '' '' ORDER BY t.key) FROM (' || q || ') t WHERE t.period = ''none''' INTO r;
  RETURN r;
END $$;
SELECT name, none_rows(report(expr, 'avg(value) AS value, sum(value) AS total, count(*) FILTER (WHERE value >= 8) AS high')) FROM cases ORDER BY name;

DROP TABLE cases;
DROP FUNCTION report(text, text), pushed(text), none_rows(text), check_query(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
