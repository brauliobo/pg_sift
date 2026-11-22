-- sift.approx_percentile / sift.approx_quantiles over a sift_col index with sketches = 'kll(...)' (Stage 5B: runs in the regression of the integrated tree; needs the extension and the
-- sketches reloption of sift_col). Every check is an ASSERT, so a failure stops the script with the line; the last query prints one 'ok' row per group.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
DROP TABLE IF EXISTS kq CASCADE;
CREATE TABLE kq (id int8 PRIMARY KEY, v float8, n numeric(12, 3), d date, ts timestamp, g int4);
INSERT INTO kq
SELECT i, CASE WHEN i % 1000 = 0 THEN NULL WHEN i % 997 = 0 THEN 'NaN' ELSE (i * 7919 % 100003)::float8 END, (i * 31 % 10007) / 1000.0, DATE '2020-01-01' + (i % 3000)::int,
       TIMESTAMP '2020-01-01' + (i % 100000) * interval '1 minute', (i % 5)::int4
FROM generate_series(1, 200000) i;
CREATE INDEX kq_col ON kq USING sift_col (id, v, n, d, ts, g) WITH (sketches = 'kll(v, n(k = 400), d, ts)');

-- 1. the sketch is within 1% of the rank of the exact answer, for every fraction asked
DO $$
DECLARE ps float8[] := ARRAY[0, 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 1];
        got float8[]; want float8[]; r_got float8; r_want float8; err float8;
BEGIN
  got  := sift.approx_quantiles('kq_col', 'v', ps);
  want := sift.approx_quantiles('kq_col', 'v', ps, 'true');
  FOR i IN 1 .. array_length(ps, 1) LOOP
    SELECT count(*) FILTER (WHERE v <= got[i])::float8 / count(*) INTO r_got FROM kq WHERE v IS NOT NULL AND v <> 'NaN';
    SELECT count(*) FILTER (WHERE v <= want[i])::float8 / count(*) INTO r_want FROM kq WHERE v IS NOT NULL AND v <> 'NaN';
    err := abs(r_got - r_want);
    ASSERT err <= 0.01 OR (i IN (1, array_length(ps, 1)) AND got[i] = want[i]), format('fraction %s: rank error %s', ps[i], err);
  END LOOP;
  ASSERT sift.approx_percentile('kq_col', 'v', 0) = want[1] AND sift.approx_percentile('kq_col', 'v', 1) = want[array_length(ps, 1)], 'the extremes are exact';
END $$;

-- 2. the table name works as the index name; every sketched type answers; a column without a sketch answers exactly
DO $$
BEGIN
  ASSERT sift.approx_percentile('kq', 'v', 0.5) = sift.approx_percentile('kq_col', 'v', 0.5), 'table and index agree';
  ASSERT sift.approx_percentile('kq', 'n', 0.5) IS NOT NULL AND sift.approx_percentile('kq', 'd', 0.5) IS NOT NULL AND sift.approx_percentile('kq', 'ts', 0.5) IS NOT NULL;
  ASSERT sift.approx_percentile('kq', 'g', 0.5) = sift.approx_percentile('kq', 'g', 0.5, 'true'), 'g has no sketch: exact';
  ASSERT sift.approx_percentile('kq', 'id', 0.5) = 100000, 'id has no sketch: exact';
END $$;

-- 3. a filter is always exact
DO $$
BEGIN
  ASSERT sift.approx_percentile('kq', 'v', 0.5, 'g = 3') = (SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY v) FROM kq WHERE g = 3 AND v <> 'NaN'), 'filtered answer is percentile_disc';
  ASSERT sift.approx_quantiles('kq', 'v', ARRAY[0.1, 0.9], 'g = 3') = (SELECT percentile_disc(ARRAY[0.1, 0.9]) WITHIN GROUP (ORDER BY v) FROM kq WHERE g = 3 AND v <> 'NaN');
  ASSERT sift.approx_percentile('kq', 'v', 0.5, 'false') IS NULL, 'no rows: NULL';
END $$;

-- 4. more than 1% dead rows: the answer is exact
DELETE FROM kq WHERE id <= 10000 AND v IS NOT NULL;
DO $$
DECLARE exact float8;
BEGIN
  SELECT percentile_disc(0.9) WITHIN GROUP (ORDER BY v) INTO exact FROM kq WHERE v <> 'NaN';
  ASSERT sift.approx_percentile('kq', 'v', 0.9) = exact, '5% dead rows: exact';
END $$;

-- 5. a row security policy, a missing privilege and bad arguments
DO $$
BEGIN
  BEGIN PERFORM sift.approx_percentile('kq', 'v', 1.5); ASSERT false; EXCEPTION WHEN numeric_value_out_of_range THEN NULL; END;
  BEGIN PERFORM sift.approx_percentile('kq', 'v', 'NaN'); ASSERT false; EXCEPTION WHEN numeric_value_out_of_range THEN NULL; END;
  BEGIN PERFORM sift.approx_quantiles('kq', 'v', ARRAY[0.5, NULL]); ASSERT false; EXCEPTION WHEN null_value_not_allowed THEN NULL; END;
  BEGIN PERFORM sift.approx_percentile('kq', 'nope', 0.5); ASSERT false; EXCEPTION WHEN undefined_column THEN NULL; END;
  BEGIN PERFORM sift.approx_percentile('kq', 'id', NULL); ASSERT false; EXCEPTION WHEN null_value_not_allowed THEN NULL; END;
  ASSERT sift.approx_quantiles('kq', 'v', ARRAY[]::float8[]) = ARRAY[]::float8[];
END $$;

ALTER TABLE kq ENABLE ROW LEVEL SECURITY;
CREATE POLICY kq_g ON kq USING (g = 1);
CREATE ROLE kq_reader NOLOGIN;
GRANT SELECT ON kq TO kq_reader;
SET ROLE kq_reader;
DO $$
BEGIN
  ASSERT sift.approx_percentile('kq', 'v', 0.5) = (SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY v) FROM kq WHERE v <> 'NaN'), 'a policy: the answer is of the rows the role sees';
END $$;
RESET ROLE;
REVOKE SELECT ON kq FROM kq_reader;
SET ROLE kq_reader;
DO $$
BEGIN
  BEGIN PERFORM sift.approx_percentile('kq', 'v', 0.5); ASSERT false; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET ROLE;
DROP TABLE kq CASCADE;
DROP ROLE kq_reader;
SELECT 'ok' AS kll_quantiles;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
