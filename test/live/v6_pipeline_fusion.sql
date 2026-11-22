-- v6_pipeline_fusion.sql - differential parity and plan-shape suite for the live Format 2.0 v6 operators (top-N, window, stream, fused arms).
-- Every query runs with sift.enable_agg = on and off; the sorted rows must hash equal and the plan with it on must carry the node named.
-- Output: one "name | true" line per check; test/live/v6_pipeline_fusion.out is all true. Run: psql -X -v ON_ERROR_STOP=1 -f v6_pipeline_fusion.sql <db with the extension>
\set QUIET on
\pset format unaligned
\pset tuples_only on
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET TIME ZONE 'UTC';

DROP TABLE IF EXISTS records, snapshots, strata, ticks, bins, ticks4 CASCADE;
CREATE TABLE records (
  id bigint PRIMARY KEY, dim_a_id int NOT NULL, dim_b_id int NOT NULL, item_id int NOT NULL, recorded_on timestamptz NOT NULL, started_at timestamptz NOT NULL,
  deleted_at timestamptz, score float8, amount numeric(9,3), geo_code text COLLATE "C" NOT NULL, tier_score int, currency_code text COLLATE "C" NOT NULL,
  metrics_jsonb jsonb, analysis_jsonb jsonb);
CREATE TABLE snapshots (id bigint PRIMARY KEY, dim_a_id int NOT NULL, item_id int NOT NULL, dim_b_id int NOT NULL, currency_code text COLLATE "C" NOT NULL, month date NOT NULL, amount numeric(9,3));

INSERT INTO records
SELECT g, 1 + g % 12, 1 + g % 7, 1 + g % 5, TIMESTAMPTZ '2026-01-01' + (g * 37 % 86400 * 150) * interval '1 second',
       TIMESTAMPTZ '2026-01-01' + g * interval '17 minutes', CASE WHEN g % 11 = 0 THEN TIMESTAMPTZ '2026-02-01' END,
       CASE WHEN g % 13 = 0 THEN NULL ELSE (g * 31 % 997) / 7.0 END, CASE WHEN g % 17 = 0 THEN NULL ELSE (g * 29 % 9973) / 1000.0 END,
       (ARRAY['br', 'us', 'de', 'jp', 'fr', 'in'])[1 + g % 6], CASE WHEN g % 9 = 0 THEN NULL ELSE g * 53 % 100 END, (ARRAY['BRL', 'USD', 'EUR'])[1 + g % 3],
       jsonb_build_object('mhead', 'h' || g % 5, 'n', g % 100),
       CASE WHEN g % 4 = 0 THEN NULL ELSE jsonb_build_object('lang', (ARRAY['pt', 'en', 'es'])[1 + g % 3], 'normalized', g % 2 = 0) END
FROM generate_series(1, 24000) g;
INSERT INTO snapshots
SELECT row_number() OVER (), a, i, b, c, DATE '2025-01-01' + m * 31, ((a * 7 + i * 3 + b + m * m) % 5000) / 3.0
FROM generate_series(1, 6) a, generate_series(1, 3) i, generate_series(1, 2) b, unnest(ARRAY['BRL', 'USD']) c, generate_series(0, 8) m;

CREATE INDEX records_col ON records USING sift_col (id, dim_a_id, dim_b_id, item_id, recorded_on, started_at, deleted_at, score, amount, geo_code, tier_score, currency_code,
  metrics_jsonb, analysis_jsonb) WITH (key_columns = 'dim_a_id', key_order = 'dim_a_id, recorded_on');
CREATE INDEX snapshots_col ON snapshots USING sift_col (id, dim_a_id, item_id, dim_b_id, currency_code, month, amount)
  WITH (key_order = 'dim_a_id, item_id, dim_b_id, currency_code, month');
CREATE TABLE strata AS SELECT id, geo_code, tier_score, deleted_at FROM records;
CREATE INDEX strata_col ON strata USING sift_col (id, geo_code, tier_score, deleted_at) WITH (key_order = 'geo_code');
CREATE TABLE ticks AS SELECT id, recorded_on::timestamp recorded_on, started_at, amount, score FROM records;
CREATE INDEX ticks_col ON ticks USING sift_col (id, recorded_on, started_at, amount, score) WITH (key_order = 'recorded_on');
CREATE TABLE bins AS SELECT id, recorded_on, started_at, amount, score FROM records;
CREATE INDEX bins_col ON bins USING sift_col (id, recorded_on, started_at, amount, score) WITH (key_order = 'started_at');
CREATE TABLE ticks4 AS SELECT id, dim_a_id, dim_b_id, item_id, recorded_on::timestamp recorded_on, score FROM records;
CREATE INDEX ticks4_col ON ticks4 USING sift_col (id, dim_a_id, dim_b_id, item_id, recorded_on, score) WITH (key_order = 'dim_a_id, dim_b_id, item_id, recorded_on');
VACUUM (ANALYZE) records, snapshots, strata, ticks, bins, ticks4;
INSERT INTO records SELECT 24000 + g, 1 + g % 12, 1 + g % 7, 1 + g % 5, TIMESTAMPTZ '2026-03-01' + g * interval '1 minute', TIMESTAMPTZ '2026-03-01',
       NULL, g / 3.0, g / 7.0, 'br', g % 100, 'BRL', '{"mhead":"h9","n":1}', NULL FROM generate_series(1, 500) g;
DELETE FROM records WHERE id % 29 = 0;
UPDATE records SET score = score + 1 WHERE id % 31 = 0;

\ir s52_helpers.sql

SET plan_cache_mode = force_generic_plan;

-- Q-A16: multi-filter top-K feed, parameters through a generic plan, seven pages
CREATE FUNCTION s52_a16(a int[], t0 timestamptz, t1 timestamptz, b int[], off int, lim int) RETURNS TABLE (id bigint, recorded_on timestamptz) LANGUAGE sql STABLE AS $f$
  SELECT r.id, r.recorded_on FROM records r
  WHERE r.deleted_at IS NULL AND r.dim_a_id = ANY (a) AND r.recorded_on BETWEEN t0 AND t1 AND r.dim_b_id = ANY (b)
  ORDER BY r.recorded_on DESC, r.id DESC LIMIT lim OFFSET off $f$;

DO $do$
DECLARE page int; same boolean := true; h1 text; h2 text;
BEGIN
  FOR page IN 0..6 LOOP
    PERFORM set_config('sift.enable_agg', 'on', true);
    SELECT md5(string_agg(x::text, ',')) INTO h1 FROM s52_a16('{1,2,3,5,8}', '2026-01-10', '2026-03-05', '{1,2,4,6}', page * 50, 50) x;
    PERFORM set_config('sift.enable_agg', 'off', true);
    SELECT md5(string_agg(x::text, ',')) INTO h2 FROM s52_a16('{1,2,3,5,8}', '2026-01-10', '2026-03-05', '{1,2,4,6}', page * 50, 50) x;
    same := same AND h1 IS NOT DISTINCT FROM h2;
  END LOOP;
  PERFORM s52_ok('Q-A16 seven generic-plan pages: same rows', same);
END $do$;

SELECT s52_check('Q-A16 literal feed', $q$SELECT id, recorded_on FROM records WHERE deleted_at IS NULL AND dim_a_id = ANY ('{1,2,3,5,8}') AND recorded_on BETWEEN '2026-01-10' AND '2026-03-05'
  AND dim_b_id = ANY ('{1,2,4,6}') ORDER BY recorded_on DESC, id DESC LIMIT 50 OFFSET 100$q$, 'SiftAggV6TopN', 'Granules Pruned');
CREATE FUNCTION s52_any(a int[], b int[], n bigint) RETURNS TABLE (id bigint, recorded_on timestamptz) LANGUAGE plpgsql STABLE AS $f$
BEGIN
  RETURN QUERY SELECT r.id, r.recorded_on FROM records r WHERE r.deleted_at IS NULL AND r.dim_a_id = ANY (a) AND r.dim_b_id = ANY (b)
    ORDER BY r.recorded_on DESC, r.id DESC LIMIT n;
END $f$;

DO $do$
DECLARE same boolean := true; h1 text; h2 text; arr int[]; arrs int[][] := ARRAY[[1,2,3],[7,7,7],[12,12,12]];
BEGIN
  FOREACH arr SLICE 1 IN ARRAY arrs LOOP
    PERFORM set_config('sift.enable_agg', 'on', true);
    SELECT md5(coalesce(string_agg(x::text, ','), '')) INTO h1 FROM s52_any(arr, '{1,2,3,4,5,6,7}', 40) x;
    PERFORM set_config('sift.enable_agg', 'off', true);
    SELECT md5(coalesce(string_agg(x::text, ','), '')) INTO h2 FROM s52_any(arr, '{1,2,3,4,5,6,7}', 40) x;
    same := same AND h1 = h2;
  END LOOP;
  PERFORM s52_ok('Q-A16 = ANY ($n) parameter arrays: same rows', same);
  PERFORM s52_ok('Q-A16 = ANY ($n) empty and NULL arrays: no rows',
                 (SELECT count(*) FROM s52_any('{}', '{1,2}', 10)) = 0 AND (SELECT count(*) FROM s52_any('{NULL}', '{1,2}', 10)) = 0);
END $do$;

PREPARE s52_pany(int[], int[], bigint) AS SELECT id FROM records WHERE deleted_at IS NULL AND dim_a_id = ANY ($1) AND dim_b_id = ANY ($2)
  ORDER BY recorded_on DESC, id DESC LIMIT $3;
SELECT s52_ok('Q-A16 = ANY ($n): top-N with granules pruned by the array',
              position('SiftAggV6TopN' IN p) > 0 AND p ~ 'Granules Pruned: [1-9]')
  FROM (SELECT s52_plan($$EXECUTE s52_pany('{1,2,3,9,10}', '{1,2,3,4,5,6,7}', 20)$$) p) x;

SELECT s52_check('Q-A16 offset 0', $q$SELECT id FROM records WHERE deleted_at IS NULL ORDER BY recorded_on DESC, id DESC LIMIT 50 OFFSET 0$q$, 'SiftAggV6TopN');
SELECT s52_check('Q-A16 limit 0', $q$SELECT id FROM records ORDER BY recorded_on DESC, id DESC LIMIT 0$q$);
SELECT s52_check('Q-A16 with ties', $q$SELECT id, tier_score FROM records ORDER BY tier_score DESC NULLS LAST FETCH FIRST 40 ROWS WITH TIES$q$, 'SiftAggV6TopN');
SELECT s52_check('Q-A16 beyond the tournament bound', $q$SELECT id FROM records ORDER BY recorded_on, id LIMIT 10 OFFSET 100000$q$);
SELECT s52_check('Q-A16 text key', $q$SELECT id FROM records WHERE geo_code = 'br' ORDER BY geo_code, id DESC LIMIT 30$q$);

-- Q-A13: stratified sample, ROW_NUMBER <= parameter
CREATE FUNCTION s52_a13(n int) RETURNS TABLE (geo_code text, id bigint, rn bigint) LANGUAGE sql STABLE AS $f$
  SELECT * FROM (SELECT r.geo_code, r.id, row_number() OVER (PARTITION BY r.geo_code ORDER BY r.tier_score DESC NULLS LAST, r.id) rn FROM strata r WHERE r.deleted_at IS NULL) s
  WHERE rn <= n $f$;
DO $do$
DECLARE h1 text; h2 text;
BEGIN
  PERFORM set_config('sift.enable_agg', 'on', true);
  SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) INTO h1 FROM s52_a13(5) x;
  PERFORM set_config('sift.enable_agg', 'off', true);
  SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) INTO h2 FROM s52_a13(5) x;
  PERFORM s52_ok('Q-A13 parameterized run condition: same rows', h1 = h2);
END $do$;
SELECT s52_check('Q-A13 literal', $q$SELECT * FROM (SELECT geo_code, id, row_number() OVER (PARTITION BY geo_code ORDER BY tier_score DESC NULLS LAST, id) rn FROM strata
  WHERE deleted_at IS NULL) s WHERE rn <= 5$q$, 'SiftAggV6Window');
SELECT s52_check('Q-A13 lateral summary is not fused', $q$SELECT d.dim_a_id, l.n FROM (SELECT DISTINCT dim_a_id FROM records) d
  LEFT JOIN LATERAL (SELECT count(*) n FROM records r WHERE r.dim_a_id = d.dim_a_id AND r.deleted_at IS NULL) l ON true$q$);

-- Q-A11: snapshot evolution windows over an inner aggregate, DISTINCT ON
SELECT s52_check('Q-A11 numeric windows', $q$SELECT dim_a_id, item_id, dim_b_id, currency_code, month, amount, lag(amount) OVER w, first_value(amount) OVER w,
  stddev_pop(amount) OVER w, stddev_samp(amount) OVER w, var_pop(amount) OVER w, var_samp(amount) OVER w
  FROM snapshots WINDOW w AS (PARTITION BY dim_a_id, item_id, dim_b_id, currency_code ORDER BY month)$q$, 'SiftAggV6Window');
SELECT s52_check('Q-A11 inner aggregate under a window', $q$SELECT dim_a_id, month, total, lag(total) OVER (PARTITION BY dim_a_id ORDER BY month), stddev_pop(total) OVER (PARTITION BY dim_a_id ORDER BY month)
  FROM (SELECT dim_a_id, month, sum(amount) total FROM snapshots GROUP BY 1, 2) s$q$, 'SiftAggV6');
SELECT s52_check('Q-A11 DISTINCT ON', $q$SELECT DISTINCT ON (dim_a_id) dim_a_id, month, amount FROM snapshots ORDER BY dim_a_id, month DESC, id$q$);
SELECT s52_check('Q-A11 float windows', $q$SELECT id, lag(score, 2, -1) OVER w, lead(score) OVER w, stddev_samp(score) OVER w FROM records WHERE dim_a_id = 3
  WINDOW w AS (PARTITION BY dim_b_id ORDER BY recorded_on, id)$q$);

-- Q-A6: dual-branch blended CTE, one shared pass
SELECT s52_check('Q-A6 materialized base', $q$WITH base AS MATERIALIZED (SELECT id, dim_b_id, item_id, score, amount FROM records
    WHERE deleted_at IS NULL AND dim_a_id = ANY ('{1,2,3}') AND recorded_on BETWEEN '2026-01-05' AND '2026-03-20'),
  m1 AS (SELECT dim_b_id cat, count(*) c1, avg(score) a1 FROM base GROUP BY 1),
  m2 AS (SELECT dim_b_id cat, count(*) c2, sum(amount) s2 FROM base WHERE item_id = 2 GROUP BY 1)
  SELECT * FROM m1 FULL JOIN m2 USING (cat)$q$, 'Fused Arms: 2', 'Fused Keys: union');

-- Q-A9: UNION ALL peer comparison
SELECT s52_check('Q-A9 base against peers', $q$SELECT 'base' side, dim_b_id, count(*), avg(score) FROM records
    WHERE deleted_at IS NULL AND dim_a_id = 4 AND recorded_on BETWEEN '2026-01-05' AND '2026-03-20' GROUP BY dim_b_id
  UNION ALL SELECT 'peer', dim_b_id, count(*), avg(score) FROM records
    WHERE deleted_at IS NULL AND dim_a_id = ANY ('{1,2,3,5}') AND recorded_on BETWEEN '2026-01-05' AND '2026-03-20' GROUP BY dim_b_id$q$, 'Fused Keys: union', 'Fused Arms: 2');

-- streaming GROUP BY with monotonic bucket keys
SELECT s52_check('stream date_trunc week', $q$SELECT date_trunc('week', recorded_on) w, count(*), sum(amount), avg(score), min(id), max(id) FROM ticks GROUP BY 1$q$, 'SiftAggV6Stream');
SELECT s52_check('stream date_bin hour', $q$SELECT date_bin('1 hour', started_at, TIMESTAMPTZ '2026-01-01') b, count(*) FILTER (WHERE amount > 5), count(amount) FROM bins GROUP BY 1$q$, 'SiftAggV6Stream');
SELECT s52_check('stream four keys', $q$SELECT dim_a_id, dim_b_id, item_id, date_trunc('day', recorded_on), count(*), sum(score) FROM ticks4 GROUP BY 1, 2, 3, 4$q$, 'SiftAggV6Stream');

-- safety
SET TIME ZONE 'America/Sao_Paulo';
SELECT s52_check('time zone: day buckets of a timestamptz', $q$SELECT date_trunc('day', recorded_on) d, count(*) FROM records GROUP BY 1$q$);
SET TIME ZONE 'UTC';
SELECT s52_check('time zone: same query in UTC', $q$SELECT date_trunc('day', recorded_on) d, count(*) FROM records GROUP BY 1$q$);
SELECT s52_check('outer LIMIT over fused arms', $q$(SELECT dim_b_id, count(*) FROM records WHERE dim_a_id = 1 GROUP BY 1 UNION ALL SELECT dim_b_id, count(*) FROM records WHERE dim_a_id = 2 GROUP BY 1) ORDER BY 1, 2 LIMIT 5$q$);

DO $do$
DECLARE denied boolean; p text;
BEGIN
  DROP ROLE IF EXISTS s52_tenant;
  CREATE ROLE s52_tenant;
  GRANT SELECT ON records TO s52_tenant;
  ALTER TABLE records ENABLE ROW LEVEL SECURITY;
  CREATE POLICY s52_p ON records FOR SELECT TO s52_tenant USING (dim_a_id = 1);
  SET LOCAL ROLE s52_tenant;
  p := s52_plan('SELECT id FROM records ORDER BY recorded_on DESC LIMIT 20');
  denied := position('SiftAggV6' IN p) = 0;
  RESET ROLE;
  PERFORM s52_ok('row level security: no custom scan', denied);
  ALTER TABLE records DISABLE ROW LEVEL SECURITY;
END $do$;

-- rescan: the node is the inner side of a nested loop and is run once per outer row; the rows of each run are those of the heap
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT s52_check('rescan of a top-N', $q$SELECT a, t.id FROM generate_series(1, 3) a LEFT JOIN (SELECT id FROM records ORDER BY recorded_on DESC, id DESC LIMIT 5) t ON true$q$,
                 'SiftAggV6TopN', 'loops=3');
SELECT s52_check('rescan of a window', $q$SELECT a, t.* FROM generate_series(1, 3) a LEFT JOIN (SELECT dim_a_id, month, lag(amount) OVER w lg, stddev_pop(amount) OVER w sd
  FROM snapshots WINDOW w AS (PARTITION BY dim_a_id, item_id, dim_b_id, currency_code ORDER BY month)) t ON true$q$, 'SiftAggV6Window', 'loops=3');
SELECT s52_check('rescan of a stream', $q$SELECT a, t.* FROM generate_series(1, 3) a LEFT JOIN (SELECT date_trunc('day', recorded_on) d, count(*) n, sum(score) s FROM ticks GROUP BY 1) t ON true$q$,
                 'SiftAggV6Stream', 'loops=3');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

BEGIN READ ONLY;
SELECT s52_check('read only transaction', $q$SELECT id FROM records WHERE deleted_at IS NULL ORDER BY recorded_on DESC, id DESC LIMIT 20$q$, 'SiftAggV6TopN');
COMMIT;

BEGIN;
DECLARE s52_c SCROLL CURSOR FOR SELECT id FROM records ORDER BY recorded_on DESC, id DESC LIMIT 100;
\o /dev/null
FETCH 30 FROM s52_c;
FETCH BACKWARD 10 FROM s52_c;
\o
SELECT s52_ok('scroll cursor backward', true);
CLOSE s52_c;
COMMIT;

SELECT name || ' | ' || ok FROM checks ORDER BY name COLLATE "C";

-- the plans of the checks above as the server runs them (EXPLAIN ANALYZE; kept in results/)
SELECT E'== ' || n || E'\n' || s52_plan(q) FROM (VALUES
  ('Q-A16 top-N, literal arrays', $q$SELECT id, recorded_on FROM records WHERE deleted_at IS NULL AND dim_a_id = ANY ('{1,2,3,5,8}') AND recorded_on BETWEEN '2026-01-10' AND '2026-03-05'
    AND dim_b_id = ANY ('{1,2,4,6}') ORDER BY recorded_on DESC, id DESC LIMIT 50 OFFSET 100$q$),
  ('Q-A16 top-N, = ANY ($n) parameter arrays and a parameter limit', $$EXECUTE s52_pany('{1,2,3,9,10}', '{1,2,3,4,5,6,7}', 20)$$),
  ('Q-A13 window', $q$SELECT * FROM (SELECT geo_code, id, row_number() OVER (PARTITION BY geo_code ORDER BY tier_score DESC NULLS LAST, id) rn FROM strata
    WHERE deleted_at IS NULL) s WHERE rn <= 5$q$),
  ('Q-A11 numeric windows', $q$SELECT dim_a_id, item_id, dim_b_id, currency_code, month, amount, lag(amount) OVER w, first_value(amount) OVER w, stddev_pop(amount) OVER w, var_samp(amount) OVER w
    FROM snapshots WINDOW w AS (PARTITION BY dim_a_id, item_id, dim_b_id, currency_code ORDER BY month)$q$),
  ('Q-A6 fused arms of one cohort', $q$WITH base AS MATERIALIZED (SELECT id, dim_b_id, item_id, score, amount FROM records
      WHERE deleted_at IS NULL AND dim_a_id = ANY ('{1,2,3}') AND recorded_on BETWEEN '2026-01-05' AND '2026-03-20'),
    m1 AS (SELECT dim_b_id cat, count(*) c1, avg(score) a1 FROM base GROUP BY 1),
    m2 AS (SELECT dim_b_id cat, count(*) c2, sum(amount) s2 FROM base WHERE item_id = 2 GROUP BY 1)
    SELECT * FROM m1 FULL JOIN m2 USING (cat)$q$),
  ('Q-A9 fused arms of different keys', $q$SELECT 'base' side, dim_b_id, count(*), avg(score) FROM records
      WHERE deleted_at IS NULL AND dim_a_id = 4 AND recorded_on BETWEEN '2026-01-05' AND '2026-03-20' GROUP BY dim_b_id
    UNION ALL SELECT 'peer', dim_b_id, count(*), avg(score) FROM records
      WHERE deleted_at IS NULL AND dim_a_id = ANY ('{1,2,3,5}') AND recorded_on BETWEEN '2026-01-05' AND '2026-03-20' GROUP BY dim_b_id$q$),
  ('stream date_trunc week', $q$SELECT date_trunc('week', recorded_on) w, count(*), sum(amount), avg(score), min(id), max(id) FROM ticks GROUP BY 1$q$)
) v(n, q);
